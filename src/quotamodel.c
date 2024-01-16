/* -------------------------------------------------------------------------
 *
 * quotamodel.c
 *
 * This code is responsible for init disk quota model and refresh disk quota
 * model. Disk quota related Shared memory initialization is also implemented
 * in this file.
 *
 * Copyright (c) 2018-2020 Pivotal Software, Inc.
 * Copyright (c) 2020-Present VMware, Inc. or its affiliates
 *
 * IDENTIFICATION
 *		diskquota/quotamodel.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_tablespace.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "storage/ipc.h"
#include "port/atomics.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/faultinjector.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "libpq-fe.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbdisp_query.h"
#include "cdb/cdbdispatchresult.h"
#include "cdb/cdbutil.h"

#include <math.h>

#include "diskquota.h"
#include "gp_activetable.h"
#include "relation_cache.h"
#include "diskquota_guc.h"
#include "table_size.h"
#include "quota_config.h"
#include "quota.h"
#include "rejectmap.h"
#include "diskquota_util.h"
#include "ddl_message.h"
#include "diskquota_launcher.h"
#include "diskquota_center_worker.h"
#include "msg_looper.h"

int SEGCOUNT = 0;

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* functions to refresh disk quota model*/
static void refresh_disk_quota_usage(bool is_init);

static Size DiskQuotaShmemSize(void);
static void disk_quota_shmem_startup(void);
static void init_lwlocks(void);

/* ---- Functions for disk quota shared memory ---- */
/*
 * DiskQuotaShmemInit
 *		Allocate and initialize diskquota-related shared memory
 *		This function is called in _PG_init().
 */
void
init_disk_quota_shmem(void)
{
	/*
	 * Request additional shared resources.  (These are no-ops if we're not in
	 * the postmaster process.)  We'll allocate or attach to the shared
	 * resources in pgss_shmem_startup().
	 */
	RequestAddinShmemSpace(DiskQuotaShmemSize());
	/* locks for diskquota refer to init_lwlocks() for details */
#if GP_VERSION_NUM < 70000
	RequestAddinLWLocks(DiskQuotaLocksItemNumber);
#else
	RequestNamedLWLockTranche("DiskquotaLocks", DiskQuotaLocksItemNumber);
#endif /* GP_VERSION_NUM */

	request_message_looper_lock(DISKQUOTA_CENTER_WORKER_MESSAGE_LOOPER_NAME);

	/* Install startup hook to initialize our shared memory. */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook      = disk_quota_shmem_startup;
}

/*
 * DiskQuotaShmemInit hooks.
 * Initialize shared memory data and locks.
 */
static void
disk_quota_shmem_startup(void)
{
	HASHCTL hash_ctl;

	if (prev_shmem_startup_hook) (*prev_shmem_startup_hook)();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	init_lwlocks();

	/*
	 * Four shared memory data. extension_ddl_message is used to handle
	 * diskquota extension create/drop command. disk_quota_reject_map is used
	 * to store out-of-quota rejectmap. active_tables_map is used to store
	 * active tables whose disk usage is changed.
	 */
	init_shm_worker_rejectmap();

	init_shm_worker_active_tables();

	init_shm_worker_relation_cache();

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(Oid);
	hash_ctl.entrysize = sizeof(struct MonitorDBEntryStruct);

	monitored_dbid_cache =
	        DiskquotaShmemInitHash("table oid cache which shoud tracking", diskquota_max_monitored_databases,
	                               diskquota_max_monitored_databases, &hash_ctl, HASH_ELEM, DISKQUOTA_OID_HASH);

	/* only initialize ddl_message and launcher memory on master/standby. */
	if (IS_QUERY_DISPATCHER())
	{
		init_shm_ddl_message();

		init_launcher_shmem();
	}
	LWLockRelease(AddinShmemInitLock);
}

/*
 * Initialize four shared memory locks.
 * active_table_lock is used to access active table map.
 * reject_map_lock is used to access out-of-quota rejectmap.
 * extension_ddl_message_lock is used to access content of
 * extension_ddl_message.
 * extension_ddl_lock is used to avoid concurrent diskquota
 * extension ddl(create/drop) command.
 * monitored_dbid_cache_lock is used to shared `monitored_dbid_cache` on segment process.
 */
static void
init_lwlocks(void)
{
#if GP_VERSION_NUM < 70000
	diskquota_locks.active_table_lock          = LWLockAssign();
	diskquota_locks.reject_map_lock            = LWLockAssign();
	diskquota_locks.extension_ddl_message_lock = LWLockAssign();
	diskquota_locks.extension_ddl_lock         = LWLockAssign();
	diskquota_locks.monitored_dbid_cache_lock  = LWLockAssign();
	diskquota_locks.relation_cache_lock        = LWLockAssign();
	diskquota_locks.dblist_lock                = LWLockAssign();
	diskquota_locks.workerlist_lock            = LWLockAssign();
	diskquota_locks.altered_reloid_cache_lock  = LWLockAssign();
#else
	LWLockPadded *lock_base                    = GetNamedLWLockTranche("DiskquotaLocks");
	diskquota_locks.active_table_lock          = &lock_base[0].lock;
	diskquota_locks.reject_map_lock            = &lock_base[1].lock;
	diskquota_locks.extension_ddl_message_lock = &lock_base[2].lock;
	diskquota_locks.extension_ddl_lock         = &lock_base[3].lock;
	diskquota_locks.monitored_dbid_cache_lock  = &lock_base[4].lock;
	diskquota_locks.relation_cache_lock        = &lock_base[5].lock;
	diskquota_locks.dblist_lock                = &lock_base[6].lock;
	diskquota_locks.workerlist_lock            = &lock_base[7].lock;
	diskquota_locks.altered_reloid_cache_lock  = &lock_base[8].lock;
#endif /* GP_VERSION_NUM */
}

static Size
diskquota_worker_shmem_size()
{
	Size size = 0;
	return size;
}

/*
 * DiskQuotaShmemSize
 * Compute space needed for diskquota-related shared memory
 */
static Size
DiskQuotaShmemSize(void)
{
	Size size = 0;

	size = add_size(size, active_table_shmem_size());
	size = add_size(size, diskquota_rejectmap_shmem_size());
	size = add_size(size, hash_estimate_size(diskquota_max_active_tables, sizeof(DiskQuotaRelationCacheEntry)));
	size = add_size(size, hash_estimate_size(diskquota_max_active_tables, sizeof(DiskQuotaRelidCacheEntry)));
	size = add_size(size, hash_estimate_size(diskquota_max_monitored_databases,
	                                         sizeof(struct MonitorDBEntryStruct))); // monitored_dbid_cache

	if (IS_QUERY_DISPATCHER())
	{
		size = add_size(size, diskquota_ddl_message_shmem_size());
		size = add_size(size, diskquota_launcher_shmem_size());
		size = add_size(size, sizeof(pg_atomic_uint32));
		size = add_size(size, diskquota_worker_shmem_size() * diskquota_max_monitored_databases);
		size = add_size(size, diskquota_center_worker_shmem_size());
	}

	return size;
}

/* ---- Functions for disk quota model ---- */

/*
 * Reset the shared memory of diskquota worker
 *
 * Suppose a user first drops diskquota extension, then recreates it in
 * the same database, as diskquota worker will get the same memory address
 * as before.
 *
 * As the shared memory can not be recycled, so we just clean up the shared
 * memory when dropping the extension.
 * - memset diskquotaDBStatus to 0
 * - clean all items in the maps
 */
// TODO: call in center worker
void
vacuum_disk_quota_model(uint32 id)
{}

/*
 * Check whether the diskquota state is ready
 */
bool
check_diskquota_state_is_ready()
{
	bool is_ready           = false;
	bool connected          = false;
	bool pushed_active_snap = false;
	bool ret                = true;

	StartTransactionCommand();

	/*
	 * Cache Errors during SPI functions, for example a segment may be down
	 * and current SPI execute will fail. diskquota worker process should
	 * tolerate this kind of errors and continue to check at the next loop.
	 */
	PG_TRY();
	{
		if (SPI_OK_CONNECT != SPI_connect())
		{
			ereport(ERROR,
			        (errcode(ERRCODE_INTERNAL_ERROR), errmsg("[diskquota] unable to connect to execute SPI query")));
		}
		connected = true;
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed_active_snap = true;
		is_ready           = do_check_diskquota_state_is_ready();
	}
	PG_CATCH();
	{
		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();
		EmitErrorReport();
		FlushErrorState();
		ret = false;
		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();
	}
	PG_END_TRY();
	if (connected) SPI_finish();
	if (pushed_active_snap) PopActiveSnapshot();
	if (ret)
		CommitTransactionCommand();
	else
		AbortCurrentTransaction();
	return is_ready;
}

/*
 * Check whether the diskquota state is ready.
 * Throw an error or return false if it is not.
 *
 * For empty database, table diskquota.state would be ready after
 * 'CREATE EXTENSION diskquota;'. But for non-empty database,
 * user need to run UDF diskquota.init_table_size_table()
 * manually to get all the table size information and
 * store them into table diskquota.table_size
 */
bool
do_check_diskquota_state_is_ready(void)
{
	int       ret;
	TupleDesc tupdesc;
	ret = SPI_execute("select state from diskquota.state", true, 0);
	ereportif(ret != SPI_OK_SELECT, ERROR,
	          (errcode(ERRCODE_INTERNAL_ERROR),
	           errmsg("[diskquota] check diskquota state SPI_execute failed: error code %d", ret)));

	tupdesc = SPI_tuptable->tupdesc;
#if GP_VERSION_NUM < 70000
	if (SPI_processed != 1 || tupdesc->natts != 1 || ((tupdesc)->attrs[0])->atttypid != INT4OID)
#else
	if (SPI_processed != 1 || tupdesc->natts != 1 || ((tupdesc)->attrs[0]).atttypid != INT4OID)
#endif /* GP_VERSION_NUM */
	{
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
		                errmsg("[diskquota] \"diskquota.state\" is corrupted in database \"%s\","
		                       " please recreate diskquota extension",
		                       get_database_name(MyDatabaseId))));
	}

	HeapTuple tup = SPI_tuptable->vals[0];
	Datum     dat;
	int       state;
	bool      isnull;

	dat           = SPI_getbinval(tup, tupdesc, 1, &isnull);
	state         = isnull ? DISKQUOTA_UNKNOWN_STATE : DatumGetInt32(dat);
	bool is_ready = state == DISKQUOTA_READY_STATE;

	if (!is_ready && !diskquota_is_readiness_logged())
	{
		diskquota_set_readiness_logged();
		ereport(WARNING, (errmsg("[diskquota] diskquota is not ready"),
		                  errhint("please run 'SELECT diskquota.init_table_size_table();' to initialize diskquota")));
	}
	return is_ready;
}

/*
 * Diskquota worker will refresh disk quota model
 * periodically. It will reload quota setting and
 * recalculate the changed disk usage.
 */
void
refresh_disk_quota_model(bool is_init)
{
	SEGCOUNT = getgpsegmentCount();
	if (SEGCOUNT <= 0)
	{
		ereport(ERROR, (errmsg("[diskquota] there is no active segment, SEGCOUNT is %d", SEGCOUNT)));
	}

	if (is_init) ereport(LOG, (errmsg("[diskquota] initialize quota model started")));
	/* skip refresh model when load_quotas failed */
	refresh_disk_quota_usage(is_init);
	if (is_init) ereport(LOG, (errmsg("[diskquota] initialize quota model finished")));
}

/*
 * Update the disk usage of namespace, role and tablespace.
 * Put the exceeded namespace and role into shared reject map.
 * Parameter 'is_init' is true when it's the first time that worker
 * process is constructing quota model.
 */
static void
refresh_disk_quota_usage(bool is_init)
{
	bool  connected                   = false;
	bool  pushed_active_snap          = false;
	bool  ret                         = true;
	HTAB *local_active_table_stat_map = NULL;

	StartTransactionCommand();

	/*
	 * Cache Errors during SPI functions, for example a segment may be down
	 * and current SPI execute will fail. diskquota worker process should
	 * tolerate this kind of errors and continue to check at the next loop.
	 */
	PG_TRY();
	{
		if (SPI_OK_CONNECT != SPI_connect())
		{
			ereport(ERROR,
			        (errcode(ERRCODE_INTERNAL_ERROR), errmsg("[diskquota] unable to connect to execute SPI query")));
		}
		connected = true;
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed_active_snap = true;
		/*
		 * initialization stage all the tables are active. later loop, only the
		 * tables whose disk size changed will be treated as active
		 *
		 * local_active_table_stat_map only contains the active tables which belong
		 * to the current database.
		 */
		local_active_table_stat_map = gp_fetch_active_tables(is_init);
		bool hasActiveTable         = (hash_get_num_entries(local_active_table_stat_map) != 0);
		/* refresh quota_info_map */
		// refresh_quota_info_map();
		/* copy local reject map back to shared reject map */
		bool reject_map_changed = false;
		/*
		 * Dispatch rejectmap entries to segments to perform hard-limit.
		 * If the bgworker is in init mode, the rejectmap should be refreshed anyway.
		 * Otherwise, only when the rejectmap is changed or the active_table_list is
		 * not empty the rejectmap should be dispatched to segments.
		 */
		if (is_init || (diskquota_hardlimit && (reject_map_changed || hasActiveTable)))
			dispatch_rejectmap(local_active_table_stat_map);
		hash_destroy(local_active_table_stat_map);
	}
	PG_CATCH();
	{
		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();
		EmitErrorReport();
		FlushErrorState();
		ret = false;
		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();
	}
	PG_END_TRY();
	if (connected) SPI_finish();
	if (pushed_active_snap) PopActiveSnapshot();
	if (ret)
		CommitTransactionCommand();
	else
		AbortCurrentTransaction();

	return;
}

/*
 * Given relation's oid or relfilenode, check whether the
 * quota limits of schema or owner are reached. Do enforcement
 * if the quota exceeds.
 */
bool
quota_check_common(Oid reloid, RelFileNode *relfilenode)
{
	bool enable_hardlimit;

	if (!IsTransactionState()) return true;

	if (diskquota_is_paused()) return true;

	if (OidIsValid(reloid)) return check_rejectmap_by_reloid(reloid);

	enable_hardlimit = diskquota_hardlimit;

#ifdef FAULT_INJECTOR
	if (SIMPLE_FAULT_INJECTOR("enable_check_quota_by_relfilenode") == FaultInjectorTypeSkip) enable_hardlimit = true;
#endif
	if (relfilenode && enable_hardlimit) return check_rejectmap_by_relfilenode(*relfilenode);

	return true;
}

void
update_monitor_db_mpp(Oid dbid, FetchTableStatType action, const char *schema)
{
	StringInfoData sql_command;
	initStringInfo(&sql_command);
	appendStringInfo(&sql_command,
	                 "SELECT %s.diskquota_fetch_table_stat(%d, '{%d}'::oid[]) FROM gp_dist_random('gp_id')", schema,
	                 action, dbid);
	/* Add current database to the monitored db cache on all segments */
	int ret = SPI_execute(sql_command.data, true, 0);
	pfree(sql_command.data);

	ereportif(ret != SPI_OK_SELECT, ERROR,
	          (errcode(ERRCODE_INTERNAL_ERROR),
	           errmsg("[diskquota] check diskquota state SPI_execute failed: error code %d", ret)));

	/* Add current database to the monitored db cache on coordinator */
	update_monitor_db(dbid, action);
}

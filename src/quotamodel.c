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

int SEGCOUNT = 0;

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* functions to maintain the quota maps */
static void refresh_quota_info_map(void);

/* functions to refresh disk quota model*/
static void refresh_disk_quota_usage(bool is_init);
static void calculate_table_disk_usage(bool is_init, HTAB *local_active_table_stat_map);

static Size DiskQuotaShmemSize(void);
static void disk_quota_shmem_startup(void);
static void init_lwlocks(void);

/*
 * Check the quota map, if the entry doesn't exist anymore,
 * remove it from the map. Otherwise, check if it has hit
 * the quota limit, if it does, add it to the rejectmap.
 */
static void
refresh_quota_info_map(void)
{
	HASH_SEQ_STATUS iter;
	QuotaInfoEntry *entry;

	hash_seq_init(&iter, quota_info_map);
	while ((entry = hash_seq_search(&iter)) != NULL)
	{
		QuotaType type    = entry->key.type;
		bool      removed = remove_expired_quota(entry);
		if (!removed && entry->limit > 0)
		{
			if (entry->size >= entry->limit)
			{
				Oid targetOid = entry->key.keys[0];
				/* when quota type is not NAMESPACE_TABLESPACE_QUOTA or ROLE_TABLESPACE_QUOTA, the tablespaceoid
				 * is set to be InvalidOid, so when we get it from map, also set it to be InvalidOid
				 */
				Oid tablespaceoid = (type == NAMESPACE_TABLESPACE_QUOTA) || (type == ROLE_TABLESPACE_QUOTA)
				                            ? entry->key.keys[1]
				                            : InvalidOid;

				bool segmentExceeded = entry->key.segid == -1 ? false : true;
				add_quota_to_rejectmap(type, targetOid, tablespaceoid, segmentExceeded);
			}
		}
	}
}

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
	bool    found;
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
	extension_ddl_message = ShmemInitStruct("disk_quota_extension_ddl_message", sizeof(ExtensionDDLMessage), &found);
	if (!found) memset((void *)extension_ddl_message, 0, sizeof(ExtensionDDLMessage));

	init_shm_worker_rejectmap();

	init_shm_worker_active_tables();

	init_shm_worker_relation_cache();

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(Oid);
	hash_ctl.entrysize = sizeof(struct MonitorDBEntryStruct);

	monitored_dbid_cache =
	        DiskquotaShmemInitHash("table oid cache which shoud tracking", diskquota_max_monitored_databases,
	                               diskquota_max_monitored_databases, &hash_ctl, HASH_ELEM, DISKQUOTA_OID_HASH);
	init_launcher_shmem();
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
	size      = add_size(size, diskquota_table_size_shmem_size());
	size      = add_size(size, diskquota_local_rejectmap_shmem_size());
	return size;
}

/*
 * DiskQuotaShmemSize
 * Compute space needed for diskquota-related shared memory
 */
static Size
DiskQuotaShmemSize(void)
{
	Size size;
	size = sizeof(ExtensionDDLMessage);
	size = add_size(size, active_table_shmem_size());
	size = add_size(size, diskquota_rejectmap_shmem_size());
	size = add_size(size, hash_estimate_size(diskquota_max_active_tables, sizeof(DiskQuotaRelationCacheEntry)));
	size = add_size(size, hash_estimate_size(diskquota_max_active_tables, sizeof(DiskQuotaRelidCacheEntry)));
	size = add_size(size, hash_estimate_size(diskquota_max_monitored_databases,
	                                         sizeof(struct MonitorDBEntryStruct))); // monitored_dbid_cache

	if (IS_QUERY_DISPATCHER())
	{
		size = add_size(size, diskquota_launcher_shmem_size());
		size = add_size(size, sizeof(pg_atomic_uint32));
		size = add_size(size, diskquota_worker_shmem_size() * diskquota_max_monitored_databases);
		size = add_size(size, quota_info_map_shmem_size());
	}

	return size;
}

/* ---- Functions for disk quota model ---- */
/*
 * Init disk quota model when the worker process firstly started.
 */
void
init_disk_quota_model(uint32 id)
{
	/* for table_size_map */
	init_table_size_map(id);

	/* for local_reject_map */
	init_local_reject_map(id);

	/* for quota_info_map */
	init_quota_info_map(id);
}

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
void
vacuum_disk_quota_model(uint32 id)
{
	/* table_size_map */
	vacuum_table_size_map(id);

	/* localrejectmap */
	vacuum_local_reject_map(id);

	/* quota_info_map */
	vacuum_quota_info_map(id);
}

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
	if (load_quotas())
	{
		refresh_disk_quota_usage(is_init);
	}
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
		/* TODO: if we can skip the following steps when there is no active table */
		/* recalculate the disk usage of table, schema and role */
		calculate_table_disk_usage(is_init, local_active_table_stat_map);
		/* refresh quota_info_map */
		refresh_quota_info_map();
		/* flush local table_size_map to user table table_size */
		flush_to_table_size();
		/* copy local reject map back to shared reject map */
		bool reject_map_changed = flush_local_reject_map();
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

static List *
merge_uncommitted_table_to_oidlist(List *oidlist)
{
	HASH_SEQ_STATUS              iter;
	DiskQuotaRelationCacheEntry *entry;

	if (relation_cache == NULL)
	{
		return oidlist;
	}

	remove_committed_relation_from_cache();

	LWLockAcquire(diskquota_locks.relation_cache_lock, LW_SHARED);
	hash_seq_init(&iter, relation_cache);
	while ((entry = hash_seq_search(&iter)) != NULL)
	{
		/* The session of db1 should not see the table inside db2. */
		if (entry->primary_table_relid == entry->relid && entry->rnode.node.dbNode == MyDatabaseId)
		{
			oidlist = lappend_oid(oidlist, entry->relid);
		}
	}
	LWLockRelease(diskquota_locks.relation_cache_lock);

	return oidlist;
}

/*
 *  Incremental way to update the disk quota of every database objects
 *  Recalculate the table's disk usage when it's a new table or active table.
 *  Detect the removed table if it's no longer in pg_class.
 *  If change happens, no matter size change or owner change,
 *  update namespace_size_map and role_size_map correspondingly.
 *  Parameter 'is_init' set to true at initialization stage to fetch tables
 *  size from table table_size
 */

static void
calculate_table_disk_usage(bool is_init, HTAB *local_active_table_stat_map)
{
	bool                table_size_map_found;
	bool                active_tbl_found;
	int64               updated_total_size;
	TableSizeEntry     *tsentry = NULL;
	Oid                 relOid;
	HASH_SEQ_STATUS     iter;
	ActiveTableEntry   *active_table_entry;
	TableSizeEntryKey   key;
	ActiveTableEntryKey active_table_key;
	List               *oidlist;
	ListCell           *l;

	/*
	 * unset is_exist flag for tsentry in table_size_map this is used to
	 * detect tables which have been dropped.
	 */
	hash_seq_init(&iter, table_size_map);
	while ((tsentry = hash_seq_search(&iter)) != NULL)
	{
		reset_table_size_entry_flag(tsentry, TABLE_EXIST);
	}

	/*
	 * scan pg_class to detect table event: drop, reset schema, reset owner.
	 * calculate the file size for active table and update namespace_size_map
	 * and role_size_map
	 */
	oidlist = get_rel_oid_list();

	oidlist = merge_uncommitted_table_to_oidlist(oidlist);

	foreach (l, oidlist)
	{
		HeapTuple     classTup;
		Form_pg_class classForm     = NULL;
		Oid           relnamespace  = InvalidOid;
		Oid           relowner      = InvalidOid;
		Oid           reltablespace = InvalidOid;
		relOid                      = lfirst_oid(l);

		classTup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relOid));
		if (HeapTupleIsValid(classTup))
		{
			classForm     = (Form_pg_class)GETSTRUCT(classTup);
			relnamespace  = classForm->relnamespace;
			relowner      = classForm->relowner;
			reltablespace = classForm->reltablespace;

			if (!OidIsValid(reltablespace))
			{
				reltablespace = MyDatabaseTableSpace;
			}
		}
		else
		{
			LWLockAcquire(diskquota_locks.relation_cache_lock, LW_SHARED);
			DiskQuotaRelationCacheEntry *relation_entry = hash_search(relation_cache, &relOid, HASH_FIND, NULL);
			if (relation_entry == NULL)
			{
				elog(WARNING, "cache lookup failed for relation %u", relOid);
				LWLockRelease(diskquota_locks.relation_cache_lock);
				continue;
			}
			relnamespace  = relation_entry->namespaceoid;
			relowner      = relation_entry->owneroid;
			reltablespace = relation_entry->rnode.node.spcNode;
			LWLockRelease(diskquota_locks.relation_cache_lock);
		}

		/*
		 * The segid is the same as the content id in gp_segment_configuration
		 * and the content id is continuous, so it's safe to use SEGCOUNT
		 * to get segid.
		 */
		for (int cur_segid = -1; cur_segid < SEGCOUNT; cur_segid++)
		{
			key.reloid = relOid;
			key.id     = TableSizeEntryId(cur_segid);

			tsentry = (TableSizeEntry *)hash_search(table_size_map, &key, HASH_ENTER, &table_size_map_found);

			if (!table_size_map_found)
			{
				tsentry->key.reloid = relOid;
				tsentry->key.id     = key.id;
				Assert(TableSizeEntrySegidStart(tsentry) == cur_segid);
				memset(tsentry->totalsize, 0, sizeof(tsentry->totalsize));
				tsentry->owneroid      = InvalidOid;
				tsentry->namespaceoid  = InvalidOid;
				tsentry->tablespaceoid = InvalidOid;
				tsentry->flag          = 0;

				int seg_st = TableSizeEntrySegidStart(tsentry);
				int seg_ed = TableSizeEntrySegidEnd(tsentry);
				for (int j = seg_st; j < seg_ed; j++) TableSizeEntrySetFlushFlag(tsentry, j);
			}

			/* mark tsentry is_exist */
			if (tsentry) set_table_size_entry_flag(tsentry, TABLE_EXIST);
			active_table_key.reloid = relOid;
			active_table_key.segid  = cur_segid;
			active_table_entry      = (ActiveTableEntry *)hash_search(local_active_table_stat_map, &active_table_key,
			                                                          HASH_FIND, &active_tbl_found);

			/* skip to recalculate the tables which are not in active list */
			if (active_tbl_found)
			{
				if (cur_segid == -1)
				{
					/* pretend process as utility mode, and append the table size on master */
					Gp_role = GP_ROLE_UTILITY;

					active_table_entry->tablesize += calculate_table_size(relOid);

					Gp_role = GP_ROLE_DISPATCH;
				}
				/* firstly calculate the updated total size of a table */
				updated_total_size = active_table_entry->tablesize - TableSizeEntryGetSize(tsentry, cur_segid);

				/* update the table_size entry */
				TableSizeEntrySetSize(tsentry, cur_segid, active_table_entry->tablesize);
				TableSizeEntrySetFlushFlag(tsentry, cur_segid);

				/* update the disk usage, there may be entries in the map whose keys are InvlidOid as the tsentry does
				 * not exist in the table_size_map */
				update_size_for_quota(updated_total_size, NAMESPACE_QUOTA, (Oid[]){tsentry->namespaceoid}, cur_segid);
				update_size_for_quota(updated_total_size, ROLE_QUOTA, (Oid[]){tsentry->owneroid}, cur_segid);
				update_size_for_quota(updated_total_size, ROLE_TABLESPACE_QUOTA,
				                      (Oid[]){tsentry->owneroid, tsentry->tablespaceoid}, cur_segid);
				update_size_for_quota(updated_total_size, NAMESPACE_TABLESPACE_QUOTA,
				                      (Oid[]){tsentry->namespaceoid, tsentry->tablespaceoid}, cur_segid);
			}
			/* table size info doesn't need to flush at init quota model stage */
			if (is_init)
			{
				TableSizeEntryResetFlushFlag(tsentry, cur_segid);
			}

			/* if schema change, transfer the file size */
			if (tsentry->namespaceoid != relnamespace)
			{
				transfer_table_for_quota(TableSizeEntryGetSize(tsentry, cur_segid), NAMESPACE_QUOTA,
				                         (Oid[]){tsentry->namespaceoid}, (Oid[]){relnamespace}, cur_segid);
			}
			/* if owner change, transfer the file size */
			if (tsentry->owneroid != relowner)
			{
				transfer_table_for_quota(TableSizeEntryGetSize(tsentry, cur_segid), ROLE_QUOTA,
				                         (Oid[]){tsentry->owneroid}, (Oid[]){relowner}, cur_segid);
			}

			if (tsentry->tablespaceoid != reltablespace || tsentry->namespaceoid != relnamespace)
			{
				transfer_table_for_quota(TableSizeEntryGetSize(tsentry, cur_segid), NAMESPACE_TABLESPACE_QUOTA,
				                         (Oid[]){tsentry->namespaceoid, tsentry->tablespaceoid},
				                         (Oid[]){relnamespace, reltablespace}, cur_segid);
			}
			if (tsentry->tablespaceoid != reltablespace || tsentry->owneroid != relowner)
			{
				transfer_table_for_quota(TableSizeEntryGetSize(tsentry, cur_segid), ROLE_TABLESPACE_QUOTA,
				                         (Oid[]){tsentry->owneroid, tsentry->tablespaceoid},
				                         (Oid[]){relowner, reltablespace}, cur_segid);
			}

			if (cur_segid == (TableSizeEntrySegidEnd(tsentry) - 1))
			{
				tsentry->namespaceoid  = relnamespace;
				tsentry->owneroid      = relowner;
				tsentry->tablespaceoid = reltablespace;
			}
		}
		if (HeapTupleIsValid(classTup))
		{
			heap_freetuple(classTup);
		}
	}

	list_free(oidlist);

	/*
	 * Process removed tables. Reduce schema and role size firstly. Remove
	 * table from table_size_map in flush_to_table_size() function later.
	 */
	hash_seq_init(&iter, table_size_map);
	while ((tsentry = hash_seq_search(&iter)) != NULL)
	{
		if (!get_table_size_entry_flag(tsentry, TABLE_EXIST))
		{
			int seg_st = TableSizeEntrySegidStart(tsentry);
			int seg_ed = TableSizeEntrySegidEnd(tsentry);
			for (int i = seg_st; i < seg_ed; i++)
			{
				update_size_for_quota(-TableSizeEntryGetSize(tsentry, i), NAMESPACE_QUOTA,
				                      (Oid[]){tsentry->namespaceoid}, i);
				update_size_for_quota(-TableSizeEntryGetSize(tsentry, i), ROLE_QUOTA, (Oid[]){tsentry->owneroid}, i);
				update_size_for_quota(-TableSizeEntryGetSize(tsentry, i), ROLE_TABLESPACE_QUOTA,
				                      (Oid[]){tsentry->owneroid, tsentry->tablespaceoid}, i);
				update_size_for_quota(-TableSizeEntryGetSize(tsentry, i), NAMESPACE_TABLESPACE_QUOTA,
				                      (Oid[]){tsentry->namespaceoid, tsentry->tablespaceoid}, i);
			}
		}
	}
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

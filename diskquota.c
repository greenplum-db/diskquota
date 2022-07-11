/* -------------------------------------------------------------------------
 *
 * diskquota.c
 *
 * Diskquota is used to limit the amount of disk space that a schema or a role
 * can use. Diskquota is based on background worker framework. It contains a
 * launcher process which is responsible for starting/refreshing the diskquota
 * worker processes which monitor given databases.
 *
 * Copyright (c) 2018-2020 Pivotal Software, Inc.
 * Copyright (c) 2020-Present VMware, Inc. or its affiliates
 *
 * IDENTIFICATION
 *		diskquota/diskquota.c
 *
 * -------------------------------------------------------------------------
 */
#include "diskquota.h"
#include "gp_activetable.h"

#include "postgres.h"

#include "funcapi.h"
#include "access/xact.h"
#include "cdb/cdbvars.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "tcop/idle_resource_cleaner.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/faultinjector.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

#define DISKQUOTA_DB "diskquota"
#define DISKQUOTA_APPLICATION_NAME "gp_reserved_gpdiskquota"

/* clang-format off */
#if !defined(DISKQUOTA_VERSION) || \
	!defined(DISKQUOTA_MAJOR_VERSION) || \
	!defined(DISKQUOTA_PATCH_VERSION) || \
	!defined(DISKQUOTA_MINOR_VERSION) || \
	!defined(DISKQUOTA_BINARY_NAME)
	#error Version not found. Please check if the VERSION file exists.
#endif
/* clang-format on */

#include <unistd.h>                 // for useconds_t
extern int usleep(useconds_t usec); // in <unistd.h>

/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup  = false;
static volatile sig_atomic_t got_sigterm = false;
static volatile sig_atomic_t got_sigusr1 = false;
static volatile sig_atomic_t got_sigusr2 = false;

/* GUC variables */
int  diskquota_naptime           = 0;
int  diskquota_max_active_tables = 0;
int  diskquota_worker_timeout    = 60; /* default timeout is 60 seconds */
bool diskquota_hardlimit         = false;
int  diskquota_max_workers       = 10;

DiskQuotaLocks       diskquota_locks;
ExtensionDDLMessage *extension_ddl_message = NULL;

/* For each diskquota worker */
static DiskQuotaWorkerEntry *MyWorkerInfo = NULL;
/* using hash table to support incremental update the table size entry.*/
static int                           num_db = 0;
static DiskquotaLauncherShmemStruct *DiskquotaLauncherShmem;
/* the current db to be run */
static dlist_node   *curDB = NULL;
static MemoryContext LauncherCtx;
DiskquotaDBStatus   *diskquotaDBStatus;
// static ResourceOwner diskquotaResourceOwner;

/* functions of disk quota*/
void _PG_init(void);
void _PG_fini(void);
void disk_quota_worker_main(Datum);
void disk_quota_launcher_main(Datum);

static void disk_quota_sigterm(SIGNAL_ARGS);
static void disk_quota_sighup(SIGNAL_ARGS);
static void define_guc_variables(void);
static bool start_worker(void);
static void create_monitor_db_table(void);
static void add_dbid_to_database_list(Oid dbid);
static void del_dbid_from_database_list(Oid dbid);
static void process_extension_ddl_message(void);
static void do_process_extension_ddl_message(MessageResult *code, ExtensionDDLMessage local_extension_ddl_message);
static void terminate_all_workers(void);
static void on_add_db(Oid dbid, MessageResult *code);
static void on_del_db(Oid dbid, MessageResult *code);
static bool is_valid_dbid(Oid dbid);
extern void invalidate_database_rejectmap(Oid dbid);
static void FreeWorkerOnExit(int code, Datum arg);
static void FreeWorker(DiskQuotaWorkerEntry *worker);
static void init_database_list(void);
static bool CanLaunchWorker(void);
static DiskquotaDBEntry     *next_db();
static DiskQuotaWorkerEntry *next_worker(DiskquotaDBEntry *dbentry);
static DiskquotaDBEntry     *add_db_entry(Oid dbid);
static void                  release_db_entry(Oid dbid);
static bool                  is_new_db(Oid dbid);

bool
diskquota_is_paused()
{
	Assert(MyDatabaseId != InvalidOid);
	bool           paused = false;
	bool           found;
	MonitorDBEntry entry;

	LWLockAcquire(diskquota_locks.monitoring_dbid_cache_lock, LW_SHARED);
	entry = hash_search(monitoring_dbid_cache, &MyDatabaseId, HASH_FIND, &found);
	if (found)
	{
		paused = entry->paused;
	}
	LWLockRelease(diskquota_locks.monitoring_dbid_cache_lock);
	return paused;
}

/*
 * DiskquotaLauncherShmemSize
 *		Compute space needed for diskquota launcher related shared memory
 */
Size
DiskquotaLauncherShmemSize(void)
{
	Size size;

	/*
	 * Need the fixed struct and the array of WorkerInfoData.
	 */
	size = sizeof(DiskquotaLauncherShmemStruct);
	size = MAXALIGN(size);
	size = add_size(size, mul_size(diskquota_max_workers, sizeof(struct DiskQuotaWorkerEntry)));
	size = MAXALIGN(size);
	size = add_size(size, mul_size(MAX_NUM_MONITORED_DB, sizeof(struct DiskquotaDBEntry)));
	return size;
}
/*
 * Entrypoint of diskquota module.
 *
 * Init shared memory and hooks.
 * Define GUCs.
 * start diskquota launcher process.
 */
void
_PG_init(void)
{
	/* diskquota.so must be in shared_preload_libraries to init SHM. */
	if (!process_shared_preload_libraries_in_progress)
	{
		ereport(ERROR, (errmsg("[diskquota] booting " DISKQUOTA_VERSION ", but " DISKQUOTA_BINARY_NAME
		                       " not in shared_preload_libraries. abort.")));
	}
	else
	{
		ereport(INFO, (errmsg("booting diskquota-" DISKQUOTA_VERSION)));
	}

	BackgroundWorker worker;
	memset(&worker, 0, sizeof(BackgroundWorker));

	/* values are used in later calls */
	define_guc_variables();

	init_disk_quota_shmem();
	init_disk_quota_enforcement();
	init_active_table_hook();

	/* Add dq_object_access_hook to handle drop extension event. */
	register_diskquota_object_access_hook();

	/* start disk quota launcher only on master */
	if (!IS_QUERY_DISPATCHER())
	{
		return;
	}

	/* set up common data for diskquota launcher worker */
	worker.bgw_flags      = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	/* launcher process should be restarted after pm reset. */
	worker.bgw_restart_time = BGW_DEFAULT_RESTART_INTERVAL;
	snprintf(worker.bgw_library_name, BGW_MAXLEN, DISKQUOTA_BINARY_NAME);
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "disk_quota_launcher_main");
	worker.bgw_notify_pid = 0;

	snprintf(worker.bgw_name, BGW_MAXLEN, "[diskquota] - launcher");

	RegisterBackgroundWorker(&worker);
}

void
_PG_fini(void)
{}

/*
 * Signal handler for SIGTERM
 * Set a flag to let the main loop to terminate, and set our latch to wake
 * it up.
 */
static void
disk_quota_sigterm(SIGNAL_ARGS)
{
	int save_errno = errno;

	got_sigterm = true;
	if (MyProc) SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

/*
 * Signal handler for SIGHUP
 * Set a flag to tell the main loop to reread the config file, and set
 * our latch to wake it up.
 */
static void
disk_quota_sighup(SIGNAL_ARGS)
{
	int save_errno = errno;

	got_sighup = true;
	if (MyProc) SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

/*
 * Signal handler for SIGUSR1
 * Set a flag to tell the launcher to handle extension ddl message
 */
static void
disk_quota_sigusr1(SIGNAL_ARGS)
{
	int save_errno = errno;

	got_sigusr1 = true;

	if (MyProc) SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

/*
 * Signal handler for SIGUSR2
 * Set a flag to tell the launcher to handle extension ddl message
 */
static void
disk_quota_sigusr2(SIGNAL_ARGS)
{
	int save_errno = errno;

	got_sigusr2 = true;

	if (MyProc) SetLatch(&MyProc->procLatch);

	errno = save_errno;
}
/*
 * Define GUC variables used by diskquota
 */
static void
define_guc_variables(void)
{
#if DISKQUOTA_DEBUG
	const int min_naptime = 0;
#else
	const int min_naptime = 1;
#endif

	DefineCustomIntVariable("diskquota.naptime", "Duration between each check (in seconds).", NULL, &diskquota_naptime,
	                        2, min_naptime, INT_MAX, PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("diskquota.max_active_tables", "Max number of active tables monitored by disk-quota.", NULL,
	                        &diskquota_max_active_tables, 1 * 1024 * 1024, 1, INT_MAX, PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("diskquota.worker_timeout", "Duration between each check (in seconds).", NULL,
	                        &diskquota_worker_timeout, 60, 1, INT_MAX, PGC_SIGHUP, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("diskquota.hard_limit", "Set this to 'on' to enable disk-quota hardlimit.", NULL,
	                         &diskquota_hardlimit, false, PGC_SIGHUP, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("diskquota.max_workers", "Max number or workers to run diskquota extension.", NULL,
	                        &diskquota_max_workers, 10, 1, INT_MAX, PGC_SIGHUP, 0, NULL, NULL, NULL);
}

/* ---- Functions for disk quota worker process ---- */

/*
 * Disk quota worker process will refresh disk quota model periodically.
 * Refresh logic is defined in quotamodel.c
 */
void
disk_quota_worker_main(Datum main_arg)
{
	char *dbname       = MyBgworkerEntry->bgw_name;
	int   launcher_pid = 0;
	bool  loop         = true;
	ereport(LOG, (errmsg("[diskquota] start disk quota worker process to monitor database:%s", dbname)));

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, disk_quota_sighup);
	pqsignal(SIGTERM, disk_quota_sigterm);
	pqsignal(SIGUSR1, disk_quota_sigusr1);

	/* diskquota worke relalted things: release worker and notify launcher */
	LWLockAcquire(diskquota_locks.extension_ddl_message_lock, LW_SHARED);
	launcher_pid = extension_ddl_message->launcher_pid;
	LWLockRelease(diskquota_locks.extension_ddl_message_lock);
	MyWorkerInfo = (DiskQuotaWorkerEntry *)DatumGetPointer(MyBgworkerEntry->bgw_main_arg);
	Assert(MyWorkerInfo != NULL);
	on_shmem_exit(FreeWorkerOnExit, 0);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
	BackgroundWorkerInitializeConnection(dbname, NULL);

	set_config_option("application_name", DISKQUOTA_APPLICATION_NAME, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true,
	                  0);

	/* diskquota worker should has Gp_role as dispatcher */
	Gp_role = GP_ROLE_DISPATCH;

	init_disk_quota_model(MyWorkerInfo->dbEntry->dbid);
	/*
	 * Initialize diskquota related local hash map and refresh model
	 * immediately
	 */

	// check current binary version and SQL DLL version are matched
	int times = 0;
	while (!got_sigterm)
	{
		CHECK_FOR_INTERRUPTS();

		int major = -1, minor = -1;
		int has_error = worker_spi_get_extension_version(&major, &minor) != 0;

		if (major == DISKQUOTA_MAJOR_VERSION && minor == DISKQUOTA_MINOR_VERSION) break;

		if (has_error)
		{
			static char _errfmt[] = "find issues in pg_class.pg_extension check server log. waited %d seconds",
			            _errmsg[sizeof(_errfmt) + sizeof("2147483647" /* INT_MAX */) + 1] = {};
			snprintf(_errmsg, sizeof(_errmsg), _errfmt, times * diskquota_naptime);

			init_ps_display("bgworker:", "[diskquota]", dbname, _errmsg);
		}
		else
		{
			init_ps_display("bgworker:", "[diskquota]", dbname,
			                "v" DISKQUOTA_VERSION " is not matching with current SQL. stop working");
		}

		ereportif(!has_error && times == 0, WARNING,
		          (errmsg("[diskquota] worker for \"%s\" detected the installed version is \"%d.%d\", "
		                  "but current version is %s. abort due to version not match",
		                  dbname, major, minor, DISKQUOTA_VERSION),
		           errhint("run alter extension diskquota update to \"%d.%d\"", DISKQUOTA_MAJOR_VERSION,
		                   DISKQUOTA_MINOR_VERSION)));

		int rc = WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
		                   diskquota_naptime * 1000L);
		ResetLatch(&MyProc->procLatch);
		if (rc & WL_POSTMASTER_DEATH)
		{
			ereport(LOG, (errmsg("[diskquota] bgworker for \"%s\" is being terminated by postmaster death.", dbname)));
			proc_exit(-1);
		}

		times++;
	}

	/*
	 * Set ps display name of the worker process of diskquota, so we can
	 * distinguish them quickly. Note: never mind parameter name of the
	 * function `init_ps_display`, we only want the ps name looks like
	 * 'bgworker: [diskquota] <dbname> ...'
	 */
	init_ps_display("bgworker:", "[diskquota]", dbname, "");
	/* If the diskquota extension is recreated, the related things in shared memory need to be reset */
	if (is_new_db(MyDatabaseId)) reset_disk_quota_model(MyDatabaseId);

	if (!diskquotaDBStatus->inited)
	{
		/* Waiting for diskquota state become ready */
		while (!got_sigterm)
		{
			int rc;

			CHECK_FOR_INTERRUPTS();

			/*
			 * Check whether the state is in ready mode. The state would be
			 * unknown, when you `create extension diskquota` at the first time.
			 * After running UDF init_table_size_table() The state will changed to
			 * be ready.
			 */
			if (check_diskquota_state_is_ready())
			{
				break;
			}
			rc = WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
			               diskquota_naptime * 1000L);
			ResetLatch(&MyProc->procLatch);

			// be nice to scheduler when naptime == 0 and diskquota_is_paused() == true
			if (!diskquota_naptime) usleep(1);

			/* Emergency bailout if postmaster has died */
			if (rc & WL_POSTMASTER_DEATH)
			{
				ereport(LOG,
				        (errmsg("[diskquota] bgworker for \"%s\" is being terminated by postmaster death.", dbname)));
				proc_exit(1);
			}

			/* In case of a SIGHUP, just reload the configuration. */
			if (got_sighup)
			{
				got_sighup = false;
				ProcessConfigFile(PGC_SIGHUP);
			}
		}
	}

	/* if received sigterm, just exit the worker process */
	if (got_sigterm)
	{
		ereport(LOG, (errmsg("[diskquota] bgworker for \"%s\" is being terminated by SIGTERM.", dbname)));
		/* clear the out-of-quota rejectmap in shared memory */
		invalidate_database_rejectmap(MyDatabaseId);
		proc_exit(0);
	}
	ereport(LOG, (errmsg("[diskquota] start bgworker for database: \"%s\"", dbname)));

	while (!got_sigterm && loop)
	{
		int rc;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Background workers mustn't call usleep() or any direct equivalent:
		 * instead, they may wait on their process latch, which sleeps as
		 * necessary, but is awakened if postmaster dies.  That way the
		 * background process goes away immediately in an emergency.
		 */
		rc = WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, diskquota_naptime * 1000L);
		ResetLatch(&MyProc->procLatch);

		// be nice to scheduler when naptime == 0 and diskquota_is_paused() == true
		if (!diskquota_naptime) usleep(1);

		/* Emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
		{
			ereport(LOG, (errmsg("[diskquota] bgworker for \"%s\" is being terminated by postmaster death.", dbname)));
			proc_exit(1);
		}

		/* In case of a SIGHUP, just reload the configuration. */
		if (got_sighup)
		{
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		SIMPLE_FAULT_INJECTOR("diskquota_worker_main");
		if (!diskquota_is_paused())
		{
			/* Refresh quota model with init mode */
			refresh_disk_quota_model(!diskquotaDBStatus->inited);
			if (!diskquotaDBStatus->inited) diskquotaDBStatus->inited = true;
		}
		worker_increase_epoch(MyDatabaseId);
		MemoryAccounting_Reset();
		loop = !DiskquotaLauncherShmem->dynamicWorker;
	}

	ereport(LOG, (errmsg("[diskquota] stop bgworker for database: \"%s\"", dbname)));
	proc_exit(0);
}

static inline bool
isAbnormalLoopTime(int diff_sec)
{
	int max_time;
	if (diskquota_naptime > 6)
		max_time = diskquota_naptime * 2;
	else
		max_time = diskquota_naptime + 6;
	return diff_sec > max_time;
}

/* ---- Functions for launcher process ---- */
/*
 * Launcher process manages the worker processes based on
 * GUC diskquota.monitor_databases in configuration file.
 */
void
disk_quota_launcher_main(Datum main_arg)
{
	time_t loop_begin, loop_end;

	/* establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, disk_quota_sighup);
	pqsignal(SIGTERM, disk_quota_sigterm);
	pqsignal(SIGUSR1, disk_quota_sigusr1);
	pqsignal(SIGUSR2, disk_quota_sigusr2);
	/* we're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	LWLockAcquire(diskquota_locks.extension_ddl_message_lock, LW_EXCLUSIVE);
	extension_ddl_message->launcher_pid = MyProcPid;
	LWLockRelease(diskquota_locks.extension_ddl_message_lock);
	LauncherCtx = AllocSetContextCreate(TopMemoryContext, "Diskquota Launcher", ALLOCSET_DEFAULT_MINSIZE,
	                                    ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);

	MemoryContextSwitchTo(LauncherCtx);
	/*
	 * connect to our database 'diskquota'. launcher process will exit if
	 * 'diskquota' database is not existed.
	 */
	BackgroundWorkerInitializeConnection(DISKQUOTA_DB, NULL);

	set_config_option("application_name", DISKQUOTA_APPLICATION_NAME, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true,
	                  0);

	/* diskquota launcher should has Gp_role as dispatcher */
	Gp_role = GP_ROLE_DISPATCH;

	/*
	 * use table diskquota_namespace.database_list to store diskquota enabled
	 * database.
	 */
	create_monitor_db_table();

	init_database_list();
	EnableClientWaitTimeoutInterrupt();
	StartIdleResourceCleanupTimers();
	loop_end = time(NULL);

	struct timeval nap;
	if (diskquota_naptime == 0) diskquota_naptime = 1;
	nap.tv_sec                  = diskquota_naptime;
	nap.tv_usec                 = 0;
	TimestampTz loop_start_time = GetCurrentTimestamp();
	/* main loop: do this until the SIGTERM handler tells us to terminate. */
	ereport(LOG, (errmsg("[diskquota launcher] start main loop")));
	while (!got_sigterm)
	{
		int         rc;
		TimestampTz current_time = 0;
		bool        canLaunch    = true;

		CHECK_FOR_INTERRUPTS();

		/*
		 * background workers mustn't call usleep() or any direct equivalent:
		 * instead, they may wait on their process latch, which sleeps as
		 * necessary, but is awakened if postmaster dies.  That way the
		 * background process goes away immediately in an emergency.
		 */

		if (nap.tv_sec != 0 || nap.tv_usec != 0)
		{
			rc = WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
			               (nap.tv_sec * 1000L) + (nap.tv_usec / 1000L));
			ResetLatch(&MyProc->procLatch);

			// wait at least one time slice, avoid 100% CPU usage
			// if (!diskquota_naptime) usleep(1);

			/* Emergency bailout if postmaster has died */
			if (rc & WL_POSTMASTER_DEATH)
			{
				ereport(LOG, (errmsg("[diskquota launcher] launcher is being terminated by postmaster death.")));
				proc_exit(1);
			}
		}
		/* process extension ddl message */
		if (got_sigusr2)
		{
			got_sigusr2 = false;
			CancelIdleResourceCleanupTimers();
			process_extension_ddl_message();
			StartIdleResourceCleanupTimers();
		}

		/* in case of a SIGHUP, just reload the configuration. */
		if (got_sighup)
		{
			got_sighup = false;
			CancelIdleResourceCleanupTimers();
			ProcessConfigFile(PGC_SIGHUP);
			StartIdleResourceCleanupTimers();
		}

		if (got_sigusr1)
		{
			got_sigusr1 = false;
		}
		if (curDB == NULL)
		{
			current_time = GetCurrentTimestamp();
			if (TimestampDifferenceExceeds(loop_start_time, current_time, (nap.tv_sec * 1000L) + (nap.tv_usec / 1000L)))
			{
				loop_start_time = current_time;
				nap.tv_sec      = diskquota_naptime;
				nap.tv_usec     = 0;
			}
			else
			{
				canLaunch = false;
				TimestampDifference(current_time,
				                    TimestampTzPlusMilliseconds(loop_start_time, diskquota_naptime * 1000L),
				                    &nap.tv_sec, &nap.tv_usec);
			}
		}
		if (canLaunch) start_worker();
		loop_begin = loop_end;
		loop_end   = time(NULL);
		if (isAbnormalLoopTime(loop_end - loop_begin))
		{
			ereport(WARNING, (errmsg("[diskquota launcher] loop takes too much time %d/%d",
			                         (int)(loop_end - loop_begin), diskquota_naptime)));
		}
	}

	/* terminate all the diskquota worker processes before launcher exit */
	ereport(LOG, (errmsg("[diskquota launcher] launcher is being terminated by SIGTERM.")));
	terminate_all_workers();
	proc_exit(0);
}

/*
 * Create table to record the list of monitored databases
 * we need a place to store the database with diskquota enabled
 * (via CREATE EXTENSION diskquota). Currently, we store them into
 * heap table in diskquota_namespace schema of diskquota database.
 * When database restarted, diskquota launcher will start worker processes
 * for these databases.
 */
static void
create_monitor_db_table(void)
{
	const char *sql;
	bool        connected          = false;
	bool        pushed_active_snap = false;
	bool        ret                = true;

	sql = "create schema if not exists diskquota_namespace;"
	      "create table if not exists diskquota_namespace.database_list(dbid oid not null unique);";

	StartTransactionCommand();

	/*
	 * Cache Errors during SPI functions, for example a segment may be down
	 * and current SPI execute will fail. diskquota launcher process should
	 * tolerate this kind of errors.
	 */
	PG_TRY();
	{
		int ret_code = SPI_connect();
		if (ret_code != SPI_OK_CONNECT)
		{
			ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
			                errmsg("[diskquota launcher] unable to connect to execute internal query. return code: %d.",
			                       ret_code)));
		}
		connected = true;
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed_active_snap = true;

		/* debug_query_string need to be set for SPI_execute utility functions. */
		debug_query_string = sql;

		ret_code = SPI_execute(sql, false, 0);
		if (ret_code != SPI_OK_UTILITY)
		{
			ereport(ERROR, (errmsg("[diskquota launcher] SPI_execute error, sql: \"%s\", reason: %s, ret_code: %d.",
			                       sql, strerror(errno), ret_code)));
		}
	}
	PG_CATCH();
	{
		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();
		EmitErrorReport();
		FlushErrorState();
		ret                = false;
		debug_query_string = NULL;
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

	debug_query_string = NULL;
}

/*
 * When launcher started, it will start all worker processes of
 * diskquota-enabled databases from diskquota_namespace.database_list
 */
static void
init_database_list(void)
{
	TupleDesc tupdesc;
	int       num = 0;
	int       ret;
	int       i;

	/*
	 * Don't catch errors in start_workers_from_dblist. Since this is the
	 * startup worker for diskquota launcher. If error happens, we just let
	 * launcher exits.
	 */
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		ereport(ERROR,
		        (errmsg("[diskquota launcher] SPI connect error, reason: %s, return code: %d.", strerror(errno), ret)));
	ret = SPI_execute("select dbid from diskquota_namespace.database_list;", true, 0);
	if (ret != SPI_OK_SELECT)
		ereport(ERROR,
		        (errmsg("[diskquota launcher] 'select diskquota_namespace.database_list', reason: %s, return code: %d.",
		                strerror(errno), ret)));
	tupdesc = SPI_tuptable->tupdesc;
	if (tupdesc->natts != 1 || tupdesc->attrs[0]->atttypid != OIDOID)
	{
		ereport(LOG, (errmsg("[diskquota launcher], natts/atttypid: %d.",
		                     tupdesc->natts != 1 ? tupdesc->natts : tupdesc->attrs[0]->atttypid)));
		ereport(ERROR, (errmsg("[diskquota launcher] table database_list corrupt, laucher will exit. natts: ")));
	}

	for (i = 0; i < SPI_processed; i++)
	{
		HeapTuple         tup;
		Oid               dbid;
		Datum             dat;
		bool              isnull;
		DiskquotaDBEntry *dbEntry;

		tup = SPI_tuptable->vals[i];
		dat = SPI_getbinval(tup, tupdesc, 1, &isnull);
		if (isnull) ereport(ERROR, (errmsg("[diskquota launcher] dbid cann't be null in table database_list")));
		dbid = DatumGetObjectId(dat);
		if (!is_valid_dbid(dbid))
		{
			ereport(LOG, (errmsg("[diskquota launcher] database(oid:%u) in table database_list is not a valid database",
			                     dbid)));
			continue;
		}
		dbEntry = add_db_entry(dbid);
		num++;
		/*
		 * diskquota only supports to monitor at most MAX_NUM_MONITORED_DB
		 * databases
		 */
		if (num >= MAX_NUM_MONITORED_DB)
		{
			ereport(LOG, (errmsg("[diskquota launcher] diskquota monitored database limit is reached, database(oid:%u) "
			                     "will not enable diskquota",
			                     dbid)));
			break;
		}
	}
	num_db = num;
	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
	/* TODO: clean invalid database */

	if (num_db >= diskquota_max_workers) DiskquotaLauncherShmem->dynamicWorker = true;
}

/*
 * This function is called by launcher process to handle message from other backend
 * processes which call CREATE/DROP EXTENSION diskquota; It must be able to catch errors,
 * and return an error code back to the backend process.
 */
static void
process_extension_ddl_message()
{
	MessageResult       code = ERR_UNKNOWN;
	ExtensionDDLMessage local_extension_ddl_message;

	LWLockAcquire(diskquota_locks.extension_ddl_message_lock, LW_SHARED);
	memcpy(&local_extension_ddl_message, extension_ddl_message, sizeof(ExtensionDDLMessage));
	LWLockRelease(diskquota_locks.extension_ddl_message_lock);

	/* create/drop extension message must be valid */
	if (local_extension_ddl_message.req_pid == 0 || local_extension_ddl_message.launcher_pid != MyProcPid) return;

	ereport(LOG, (errmsg("[diskquota launcher]: received create/drop extension diskquota message, extension launcher "
	                     "pid %d, %d",
	                     extension_ddl_message->launcher_pid, MyProcPid)));

	do_process_extension_ddl_message(&code, local_extension_ddl_message);

	/* Send createdrop extension diskquota result back to QD */
	LWLockAcquire(diskquota_locks.extension_ddl_message_lock, LW_EXCLUSIVE);
	memset(extension_ddl_message, 0, sizeof(ExtensionDDLMessage));
	extension_ddl_message->launcher_pid = MyProcPid;
	extension_ddl_message->result       = (int)code;
	LWLockRelease(diskquota_locks.extension_ddl_message_lock);
}

/*
 * Process 'create extension' and 'drop extension' message.
 * For 'create extension' message, store dbid into table
 * 'database_list' and start the diskquota worker process.
 * For 'drop extension' message, remove dbid from table
 * 'database_list' and stop the diskquota worker process.
 */
static void
do_process_extension_ddl_message(MessageResult *code, ExtensionDDLMessage local_extension_ddl_message)
{
	int  old_num_db         = num_db;
	bool connected          = false;
	bool pushed_active_snap = false;
	bool ret                = true;

	StartTransactionCommand();

	/*
	 * Cache Errors during SPI functions, for example a segment may be down
	 * and current SPI execute will fail. diskquota launcher process should
	 * tolerate this kind of errors.
	 */
	PG_TRY();
	{
		int ret_code = SPI_connect();
		if (ret_code != SPI_OK_CONNECT)
		{
			ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
			                errmsg("unable to connect to execute internal query. return code: %d.", ret_code)));
		}
		connected = true;
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed_active_snap = true;

		switch (local_extension_ddl_message.cmd)
		{
			case CMD_CREATE_EXTENSION:
				on_add_db(local_extension_ddl_message.dbid, code);
				num_db++;
				if (num_db >= diskquota_max_workers) DiskquotaLauncherShmem->dynamicWorker = true;
				*code = ERR_OK;
				break;
			case CMD_DROP_EXTENSION:
				on_del_db(local_extension_ddl_message.dbid, code);
				num_db--;
				if (num_db < diskquota_max_workers) DiskquotaLauncherShmem->dynamicWorker = false;
				*code = ERR_OK;
				break;
			default:
				ereport(LOG, (errmsg("[diskquota launcher]:received unsupported message cmd=%d",
				                     local_extension_ddl_message.cmd)));
				*code = ERR_UNKNOWN;
				break;
		}
	}
	PG_CATCH();
	{
		error_context_stack = NULL;
		HOLD_INTERRUPTS();
		EmitErrorReport();
		FlushErrorState();
		ret    = false;
		num_db = old_num_db;
		RESUME_INTERRUPTS();
	}
	PG_END_TRY();

	if (connected) SPI_finish();
	if (pushed_active_snap) PopActiveSnapshot();
	if (ret)
		CommitTransactionCommand();
	else
		AbortCurrentTransaction();
}

/*
 * Handle create extension diskquota
 * if we know the exact error which caused failure,
 * we set it, and error out
 */
static void
on_add_db(Oid dbid, MessageResult *code)
{
	if (num_db >= MAX_NUM_MONITORED_DB)
	{
		*code = ERR_EXCEED;
		ereport(ERROR, (errmsg("[diskquota launcher] too many databases to monitor")));
	}
	if (!is_valid_dbid(dbid))
	{
		*code = ERR_INVALID_DBID;
		ereport(ERROR, (errmsg("[diskquota launcher] invalid database oid")));
	}

	/*
	 * add dbid to diskquota_namespace.database_list set *code to
	 * ERR_ADD_TO_DB if any error occurs
	 */
	PG_TRY();
	{
		DiskquotaDBEntry *db;
		add_dbid_to_database_list(dbid);
		db = add_db_entry(dbid);
		ereport(LOG, (errmsg("[diskquota launcher] add database %s", db->dbname)));
	}
	PG_CATCH();
	{
		*code = ERR_ADD_TO_DB;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Handle message: drop extension diskquota
 * do:
 * 1. kill the associated worker process
 * 2. delete dbid from diskquota_namespace.database_list
 * 3. invalidate reject-map entries and monitoring_dbid_cache from shared memory
 */
static void
on_del_db(Oid dbid, MessageResult *code)
{
	if (!is_valid_dbid(dbid))
	{
		*code = ERR_INVALID_DBID;
		ereport(ERROR, (errmsg("[diskquota launcher] invalid database oid")));
	}

	/*
	 * delete dbid from diskquota_namespace.database_list set *code to
	 * ERR_DEL_FROM_DB if any error occurs
	 */
	PG_TRY();
	{
		del_dbid_from_database_list(dbid);
		release_db_entry(dbid);
		/* clear the out-of-quota rejectmap in shared memory */
		invalidate_database_rejectmap(dbid);
	}
	PG_CATCH();
	{
		*code = ERR_DEL_FROM_DB;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Add the database id into table 'database_list' in
 * database 'diskquota' to store the diskquota enabled
 * database info.
 */
static void
add_dbid_to_database_list(Oid dbid)
{
	int ret;

	Oid   argt[1] = {OIDOID};
	Datum argv[1] = {ObjectIdGetDatum(dbid)};

	ret = SPI_execute_with_args("select * from diskquota_namespace.database_list where dbid = $1", 1, argt, argv, NULL,
	                            true, 0);

	if (ret != SPI_OK_SELECT)
		ereport(ERROR, (errmsg("[diskquota launcher] error occured while checking database_list, "
		                       " code: %d, reason: %s.",
		                       ret, strerror(errno))));

	if (SPI_processed == 1)
	{
		ereport(WARNING, (errmsg("[diskquota launcher] database id %d is already actived, "
		                         "skip database_list update",
		                         dbid)));
		return;
	}

	ret = SPI_execute_with_args("insert into diskquota_namespace.database_list values($1)", 1, argt, argv, NULL, false,
	                            0);

	if (ret != SPI_OK_INSERT || SPI_processed != 1)
		ereport(ERROR, (errmsg("[diskquota launcher] error occured while updating database_list, "
		                       " code: %d, reason: %s.",
		                       ret, strerror(errno))));

	return;
}

/*
 * Delete database id from table 'database_list' in
 * database 'diskquota'.
 */
static void
del_dbid_from_database_list(Oid dbid)
{
	int ret;

	/* errors will be cached in outer function */
	ret = SPI_execute_with_args("delete from diskquota_namespace.database_list where dbid = $1", 1,
	                            (Oid[]){
	                                    OIDOID,
	                            },
	                            (Datum[]){
	                                    ObjectIdGetDatum(dbid),
	                            },
	                            NULL, false, 0);

	ereportif(ret != SPI_OK_DELETE, ERROR,
	          (errmsg("[diskquota launcher] del_dbid_from_database_list: reason: %s, ret_code: %d.", strerror(errno),
	                  ret)));
}

/*
 * When launcher exits, it should also terminate all the workers.
 */
static void
terminate_all_workers(void)
{
	dlist_iter        iterdb;
	DiskquotaDBEntry *dbEntry;
	LWLockAcquire(diskquota_locks.dblist_lock, LW_SHARED);
	dlist_foreach(iterdb, &DiskquotaLauncherShmem->dbList)
	{
		dbEntry = dlist_container(DiskquotaDBEntry, node, iterdb.cur);
		if (dbEntry->handle) TerminateBackgroundWorker(dbEntry->handle);
	}
	LWLockRelease(diskquota_locks.dblist_lock);
}

/*
 * Dynamically launch an disk quota worker process.
 * This function is called when laucher process receive
 * a 'create extension diskquota' message.
 */
static bool
start_worker(void)
{
	BackgroundWorker        worker;
	BackgroundWorkerHandle *handle;
	// BgwHandleStatus         status;
	MemoryContext         old_ctx;
	bool                  ret;
	DiskQuotaWorkerEntry *dq_worker;
	DiskquotaDBEntry *dbEntry;

	/* pick a db and worker to run */
	if (!CanLaunchWorker())
	{
		return false;
	}
	dbEntry = next_db();
	if (dbEntry == NULL)
	{
		return false;
	}

	dq_worker               = next_worker(dbEntry);
	if (dq_worker == NULL)
		return false;

	dbEntry->running        = true;
	memset(&worker, 0, sizeof(BackgroundWorker));
	worker.bgw_flags      = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;

	/*
	 * diskquota worker should not restart by bgworker framework. If
	 * postmaster reset, all the bgworkers will be terminated and diskquota
	 * launcher is restarted by postmaster. All the diskquota workers should
	 * be started by launcher process again.
	 */
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	sprintf(worker.bgw_library_name, DISKQUOTA_BINARY_NAME);
	sprintf(worker.bgw_function_name, "disk_quota_worker_main");

	snprintf(worker.bgw_name, sizeof(worker.bgw_name), "%s", dbEntry->dbname);
	/* set bgw_notify_pid so that we can use WaitForBackgroundWorkerStartup */
	worker.bgw_notify_pid = MyProcPid;
	worker.bgw_main_arg   = (Datum)PointerGetDatum(dq_worker);

	old_ctx = MemoryContextSwitchTo(TopMemoryContext);
	ret     = RegisterDynamicBackgroundWorker(&worker, &handle);
	MemoryContextSwitchTo(old_ctx);
	if (!ret)
	{
		FreeWorker(dq_worker);
		return false;
	}
	/* FIXME: if calling WaitForBackgroundWorkerStartup here will slow down the
	 * diskquota to register bgworkers in parallel, so just delete code here.
	 * But if postmaster failed to start bgworker, diskquota can not catch that
	 * error. It is good to wait the bgworker to startup, but I don't know
	 * where to call it.
	 */
	//	status = WaitForBackgroundWorkerStartup(handle, &pid);
	//	if (status == BGWH_STOPPED)
	//		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("could not start background process"),
	//		                errhint("More details may be available in the server log.")));
	//	if (status == BGWH_POSTMASTER_DIED)
	//		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
	//		                errmsg("cannot start background processes without postmaster"),
	//		                errhint("Kill all remaining database processes and restart the database.")));
	//
	//	Assert(status == BGWH_STARTED);

	/* Save the handle to the worker map to check the liveness. */
	dbEntry->handle = handle;

	/* no need to get lock here, because only the launcher modifies the db list */
	curDB = dlist_has_next(&DiskquotaLauncherShmem->dbList, curDB)
	                ? dlist_next_node(&DiskquotaLauncherShmem->dbList, curDB)
	                : NULL;
	return true;
}

/*
 * Check whether db oid is valid.
 */
static bool
is_valid_dbid(Oid dbid)
{
	HeapTuple tuple;

	if (dbid == InvalidOid) return false;
	tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbid));
	if (!HeapTupleIsValid(tuple)) return false;
	ReleaseSysCache(tuple);
	return true;
}

bool
worker_increase_epoch(Oid database_oid)
{
	bool found = false;
	LWLockAcquire(diskquota_locks.dblist_lock, LW_SHARED);
	DiskquotaDBEntry *db = get_db_entry(MyDatabaseId);
	if (db != NULL)
	{
		found = true;
		pg_atomic_fetch_add_u32(&(db->epoch), 1);
	}
	LWLockRelease(diskquota_locks.dblist_lock);
	return found;
}

uint32
worker_get_epoch(Oid database_oid)
{
	LWLockAcquire(diskquota_locks.dblist_lock, LW_SHARED);

	bool              found = false;
	uint32            epoch = 0;
	DiskquotaDBEntry *db    = get_db_entry(MyDatabaseId);
	if (db != NULL)
	{
		found = true;
		epoch = pg_atomic_read_u32(&(db->epoch));
	}
	if (!found)
	{
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
		                errmsg("[diskquota] worker not found for database \"%s\"", get_database_name(database_oid))));
	}
	LWLockRelease(diskquota_locks.dblist_lock);
	return epoch;
}

// Returns the worker epoch for the current database.
// An epoch marks a new iteration of refreshing quota usage by a bgworker.
// An epoch is a 32-bit unsigned integer and there is NO invalid value.
// Therefore, the UDF must throw an error if something unexpected occurs.
PG_FUNCTION_INFO_V1(show_worker_epoch);
Datum
show_worker_epoch(PG_FUNCTION_ARGS)
{
	PG_RETURN_UINT32(worker_get_epoch(MyDatabaseId));
}

static const char *
diskquota_status_check_soft_limit()
{
	// should run on coordinator only.
	Assert(IS_QUERY_DISPATCHER());
	// if worker no booted, aka 'CREATE EXTENSION' not called, diskquota is paused
	LWLockAcquire(diskquota_locks.dblist_lock, LW_SHARED);
	DiskquotaDBEntry *db = get_db_entry(MyDatabaseId);
	LWLockRelease(diskquota_locks.dblist_lock);
	if (db == NULL) return "paused";
	return diskquota_is_paused() ? "paused" : "on";
}

static const char *
diskquota_status_check_hard_limit()
{
	// should run on coordinator only.
	Assert(IS_QUERY_DISPATCHER());

	bool hardlimit = diskquota_hardlimit;

	bool paused = false;
	LWLockAcquire(diskquota_locks.dblist_lock, LW_SHARED);
	DiskquotaDBEntry *db = get_db_entry(MyDatabaseId);
	LWLockRelease(diskquota_locks.dblist_lock);
	if (db == NULL)
		paused = false;
	else
		paused = diskquota_is_paused();

	// if worker booted and 'is_paused == true' and hardlimit is enabled
	// hard limits should also paused
	if (paused && hardlimit) return "paused";

	return hardlimit ? "on" : "off";
}

static const char *
diskquota_status_binary_version()
{
	return DISKQUOTA_VERSION;
}

static const char *
diskquota_status_schema_version()
{
	static char version[64] = {0};
	memset(version, 0, sizeof(version));

	int ret = SPI_connect();
	Assert(ret = SPI_OK_CONNECT);

	ret = SPI_execute("select extversion from pg_extension where extname = 'diskquota'", true, 0);

	if (ret != SPI_OK_SELECT || SPI_processed != 1)
	{
		ereport(WARNING,
		        (errmsg("[diskquota] when reading installed version lines %ld code = %d", SPI_processed, ret)));
		goto out;
	}

	if (SPI_processed == 0)
	{
		goto out;
	}

	bool  is_null = false;
	Datum v       = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &is_null);
	Assert(is_null == false);

	char *vv = TextDatumGetCString(v);
	if (vv == NULL)
	{
		ereport(WARNING, (errmsg("[diskquota] 'extversion' is empty in pg_class.pg_extension. may catalog corrupted")));
		goto out;
	}

	StrNCpy(version, vv, sizeof(version));

out:
	SPI_finish();
	return version;
}

PG_FUNCTION_INFO_V1(diskquota_status);
Datum
diskquota_status(PG_FUNCTION_ARGS)
{
	typedef struct Context
	{
		int index;
	} Context;

	typedef struct FeatureStatus
	{
		const char *name;
		const char *(*status)(void);
	} FeatureStatus;

	static const FeatureStatus fs[] = {
	        {.name = "soft limits", .status = diskquota_status_check_soft_limit},
	        {.name = "hard limits", .status = diskquota_status_check_hard_limit},
	        {.name = "current binary version", .status = diskquota_status_binary_version},
	        {.name = "current schema version", .status = diskquota_status_schema_version},
	};

	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL())
	{
		funcctx = SRF_FIRSTCALL_INIT();

		MemoryContext oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		{
			TupleDesc tupdesc = CreateTemplateTupleDesc(2, false);
			TupleDescInitEntry(tupdesc, 1, "name", TEXTOID, -1, 0);
			TupleDescInitEntry(tupdesc, 2, "status", TEXTOID, -1, 0);
			funcctx->tuple_desc = BlessTupleDesc(tupdesc);
			Context *context    = (Context *)palloc(sizeof(Context));
			context->index      = 0;
			funcctx->user_fctx  = context;
		}
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx          = SRF_PERCALL_SETUP();
	Context *context = (Context *)funcctx->user_fctx;

	if (context->index >= sizeof(fs) / sizeof(FeatureStatus))
	{
		SRF_RETURN_DONE(funcctx);
	}

	bool  nulls[2] = {false, false};
	Datum v[2]     = {
            DirectFunctionCall1(textin, CStringGetDatum(fs[context->index].name)),
            DirectFunctionCall1(textin, CStringGetDatum(fs[context->index].status())),
    };
	ReturnSetInfo *rsi   = (ReturnSetInfo *)fcinfo->resultinfo;
	HeapTuple      tuple = heap_form_tuple(rsi->expectedDesc, v, nulls);

	context->index++;
	SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
}

static bool
check_for_timeout(TimestampTz start_time)
{
	long diff_secs  = 0;
	int  diff_usecs = 0;
	TimestampDifference(start_time, GetCurrentTimestamp(), &diff_secs, &diff_usecs);
	if (diff_secs >= diskquota_worker_timeout)
	{
		ereport(NOTICE, (errmsg("[diskquota] timeout when waiting for worker"),
		                 errhint("please check if the bgworker is still alive.")));
		return true;
	}
	return false;
}

// Checks if the bgworker for the current database works as expected.
// 1. If it returns successfully in `diskquota.naptime`, the bgworker works as expected.
// 2. If it does not terminate, there must be some issues with the bgworker.
//    In this case, we must ensure this UDF can be interrupted by the user.
PG_FUNCTION_INFO_V1(wait_for_worker_new_epoch);
Datum
wait_for_worker_new_epoch(PG_FUNCTION_ARGS)
{
	TimestampTz start_time    = GetCurrentTimestamp();
	uint32      current_epoch = worker_get_epoch(MyDatabaseId);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (check_for_timeout(start_time)) start_time = GetCurrentTimestamp();
		uint32 new_epoch = worker_get_epoch(MyDatabaseId);
		/* Unsigned integer underflow is OK */
		if (new_epoch - current_epoch >= 2u)
		{
			PG_RETURN_BOOL(true);
		}
		/* Sleep for naptime to reduce CPU usage */
		(void)WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT, diskquota_naptime ? diskquota_naptime : 1);
		ResetLatch(&MyProc->procLatch);
	}
	PG_RETURN_BOOL(false);
}

static void
FreeWorker(DiskQuotaWorkerEntry *worker)
{
	if (worker != NULL)
	{
		LWLockAcquire(diskquota_locks.workerlist_lock, LW_EXCLUSIVE);
		if (worker->dbEntry != NULL)
			worker->dbEntry->running = false;
		dlist_delete(&worker->links);
		dlist_push_head(&DiskquotaLauncherShmem->freeWorkers, &worker->links);
		DiskquotaLauncherShmem->running_workers_num--;
		LWLockRelease(diskquota_locks.workerlist_lock);
	}
}

static void
FreeWorkerOnExit(int code, Datum arg)
{
	if (MyWorkerInfo != NULL)
	{
		FreeWorker(MyWorkerInfo);
	}
}

static bool
CanLaunchWorker(void)
{
	bool result = false;
	LWLockAcquire(diskquota_locks.workerlist_lock, LW_SHARED);
	LWLockAcquire(diskquota_locks.dblist_lock, LW_SHARED);
	if (dlist_is_empty(&DiskquotaLauncherShmem->freeWorkers))
		goto out;
	if (dlist_is_empty(&DiskquotaLauncherShmem->dbList))
		goto out;
	if (DiskquotaLauncherShmem->running_workers_num >= num_db)
		goto out;
	result = true;
out:
	LWLockRelease(diskquota_locks.workerlist_lock);
	LWLockRelease(diskquota_locks.dblist_lock);
	return result;
}

void
InitLaunchShmem()
{
	bool found;
	DiskquotaLauncherShmem = (DiskquotaLauncherShmemStruct *)ShmemInitStruct("Diskquota launcher Data",
	                                                                         DiskquotaLauncherShmemSize(), &found);
	memset(DiskquotaLauncherShmem, 0, sizeof(DiskquotaLauncherShmemStruct));
	if (!found)
	{
		DiskQuotaWorkerEntry *worker;
		DiskquotaDBEntry     *db;
		int                   i;
		dlist_init(&DiskquotaLauncherShmem->freeWorkers);
		dlist_init(&DiskquotaLauncherShmem->runningWorkers);
		DiskquotaLauncherShmem->startingWorker = NULL;
		worker                                 = (DiskQuotaWorkerEntry *)((char *)DiskquotaLauncherShmem +
                                          MAXALIGN(sizeof(DiskquotaLauncherShmemStruct)));

		/* initialize the worker free list */
		for (i = 0; i < diskquota_max_workers; i++)
		{
			dlist_push_head(&DiskquotaLauncherShmem->freeWorkers, &worker[i].links);
		}
		DiskquotaLauncherShmem->running_workers_num = 0;

		db = (DiskquotaDBEntry *)((char *)DiskquotaLauncherShmem + MAXALIGN(sizeof(DiskquotaLauncherShmemStruct)) +
		                          MAXALIGN(mul_size(diskquota_max_workers, sizeof(DiskQuotaWorkerEntry))));
		dlist_init(&DiskquotaLauncherShmem->dbList);
		dlist_init(&DiskquotaLauncherShmem->freeDBList);
		for (i = 0; i < MAX_NUM_MONITORED_DB; i++)
		{
			dlist_push_head(&DiskquotaLauncherShmem->freeDBList, &db[i].node);
		}
	}
}

static DiskquotaDBEntry *
add_db_entry(Oid dbid)
{
	DiskquotaDBEntry *dbEntry = NULL;
	dlist_node       *dnode;
	LWLockAcquire(diskquota_locks.dblist_lock, LW_EXCLUSIVE);
	/* get_database_name needs to allocate memory in current memory context */
	MemoryContext oldCtx = MemoryContextSwitchTo(LauncherCtx);
	if (dlist_is_empty(&DiskquotaLauncherShmem->freeDBList))
	{
		LWLockRelease(diskquota_locks.dblist_lock);
		ereport(ERROR, (errmsg("[diskquota launcher] too many databases to monitor")));
	}
	dnode   = dlist_pop_head_node(&DiskquotaLauncherShmem->freeDBList);
	dbEntry = dlist_container(DiskquotaDBEntry, node, dnode);
	memset(dbEntry, 0, sizeof(DiskquotaDBEntry));
	dbEntry->dbid = dbid;
	/*
	 * dbEntry is in shared memory, dbname is in laucher local memory.
	 * It's ok to point to dbname in dbEntry, because laucher is a
	 * daemon process
	 *
	 */
	dbEntry->dbname = get_database_name(dbid);
	pg_atomic_write_u32(&(dbEntry->epoch), 0);
	dlist_push_tail(&DiskquotaLauncherShmem->dbList, &dbEntry->node);
	MemoryContextSwitchTo(oldCtx);
	LWLockRelease(diskquota_locks.dblist_lock);
	return dbEntry;
}

static void
release_db_entry(Oid dbid)
{
	dlist_mutable_iter iter;
	dlist_foreach_modify(iter, &DiskquotaLauncherShmem->dbList)
	{
		DiskquotaDBEntry *db = dlist_container(DiskquotaDBEntry, node, iter.cur);
		if (db->dbid == dbid)
		{
			if (curDB == iter.cur)
			{
				curDB = dlist_has_next(&DiskquotaLauncherShmem->dbList, curDB)
				                ? dlist_next_node(&DiskquotaLauncherShmem->dbList, curDB)
				                : NULL;
			}
			LWLockAcquire(diskquota_locks.dblist_lock, LW_EXCLUSIVE);
			dlist_delete(iter.cur);
			LWLockRelease(diskquota_locks.dblist_lock);
			/*
			 * When drop exention database, diskquota laucher will receive a message
			 * to kill the diskquota worker process which monitoring the target database.
			 */
			if (db->handle)
			{
				TerminateBackgroundWorker(db->handle);
				pfree(db->handle);
			}
			if (db->dbname != NULL) pfree(db->dbname);
			memset(db, 0, sizeof(DiskquotaDBEntry));
			/* put it to freeDbList */
			LWLockAcquire(diskquota_locks.dblist_lock, LW_EXCLUSIVE);
			dlist_push_tail(&DiskquotaLauncherShmem->freeDBList, iter.cur);
			LWLockRelease(diskquota_locks.dblist_lock);
			break;
		}
	}
}

/*
 * It is supposed diskquota_locks.extension_ddl_message_lock has been accquired
 * before calling this function.
 */
DiskquotaDBEntry *
get_db_entry(Oid dbid)
{
	DiskquotaDBEntry *db = NULL;
	dlist_iter        iterdb;
	dlist_foreach(iterdb, &DiskquotaLauncherShmem->dbList)
	{
		db = dlist_container(DiskquotaDBEntry, node, iterdb.cur);
		if (db->dbid == dbid) break;
		db = NULL;
	}
	return db;
}

/*
 * Pick next db to run.
 * If the curDB is NULL, pick the head db to run.
 * If the dbList empty, return NULL.
 * If the picked db is in running status, skip it, pick the next one to run.
 *
 */
static DiskquotaDBEntry *
next_db()
{
	LWLockAcquire(diskquota_locks.dblist_lock, LW_EXCLUSIVE);
	DiskquotaDBEntry *dbEntry = NULL;
	if (curDB == NULL)
	{
		curDB = dlist_head_node(&DiskquotaLauncherShmem->dbList);
		if (curDB == NULL)
		{
			goto out;
		}
	}
	dbEntry = dlist_container(DiskquotaDBEntry, node, curDB);
	while (dbEntry->running)
	{
		curDB = dlist_has_next(&DiskquotaLauncherShmem->dbList, curDB)
		                ? dlist_next_node(&DiskquotaLauncherShmem->dbList, curDB)
		                : NULL;
		if (curDB == NULL)
		{
			goto out;
		}
		dbEntry = dlist_container(DiskquotaDBEntry, node, curDB);
	}
out:
	dbEntry =  curDB == NULL ? NULL : dlist_container(DiskquotaDBEntry, node, curDB);
	LWLockRelease(diskquota_locks.dblist_lock);
	return dbEntry;
}

static DiskQuotaWorkerEntry *
next_worker(DiskquotaDBEntry *dbEntry)
{
	DiskQuotaWorkerEntry *dq_worker = NULL;
	dlist_node           *wnode;

	/* acquire worker from worker list */
	LWLockAcquire(diskquota_locks.workerlist_lock, LW_EXCLUSIVE);
	if (dlist_is_empty(&DiskquotaLauncherShmem->freeWorkers))
		goto out;
	wnode     = dlist_pop_head_node(&DiskquotaLauncherShmem->freeWorkers);
	dq_worker = dlist_container(DiskQuotaWorkerEntry, links, wnode);
	memset(dq_worker, 0, sizeof(DiskQuotaWorkerEntry));
	dq_worker->dbEntry = dbEntry;
	dlist_push_head(&DiskquotaLauncherShmem->runningWorkers, &dq_worker->links);
	DiskquotaLauncherShmem->running_workers_num++;
out:
	LWLockRelease(diskquota_locks.workerlist_lock);
	return dq_worker;
}

static bool
is_new_db(Oid dbid)
{
	bool found;
	LWLockAcquire(diskquota_locks.monitoring_dbid_cache_lock, LW_SHARED);
	hash_search(monitoring_dbid_cache, &MyDatabaseId, HASH_FIND, &found);
	LWLockRelease(diskquota_locks.monitoring_dbid_cache_lock);
	return !found;
}

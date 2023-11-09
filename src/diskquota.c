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
#include "postgres.h"

#include "funcapi.h"
#include "pgstat.h"
#include "access/xact.h"
#include "cdb/cdbgang.h"
#include "cdb/cdbvars.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/faultinjector.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/formatting.h"
#include "tcop/pquery.h"

#include "diskquota.h"
#include "gp_activetable.h"
#include "diskquota_guc.h"
#include "rejectmap.h"
#include "ddl_message.h"
#include "diskquota_launcher.h"

PG_MODULE_MAGIC;

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

DiskQuotaLocks diskquota_locks;

// Only access in diskquota worker, different from each worker.
// a pointer to DiskquotaLauncherShmem->workerEntries in shared memory
static DiskQuotaWorkerEntry *volatile MyWorkerInfo = NULL;

/* functions of disk quota*/
void _PG_init(void);
void _PG_fini(void);
void disk_quota_worker_main(Datum);

static void disk_quota_sigterm(SIGNAL_ARGS);
static void disk_quota_sighup(SIGNAL_ARGS);
static void FreeWorkerOnExit(int code, Datum arg);

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

/* ---- Functions for disk quota worker process ---- */

/*
 * Disk quota worker process will refresh disk quota model periodically.
 * Refresh logic is defined in quotamodel.c
 */
void
disk_quota_worker_main(Datum main_arg)
{
	char dbname[NAMEDATALEN];

	MyWorkerInfo = (DiskQuotaWorkerEntry *)DatumGetPointer(MyBgworkerEntry->bgw_main_arg);
	Assert(MyWorkerInfo != NULL);

	memcpy(dbname, MyWorkerInfo->dbname.data, NAMEDATALEN);

	/* Disable ORCA to avoid fallback */
	optimizer = false;

	ereport(DEBUG1, (errmsg("[diskquota] start disk quota worker process to monitor database:%s", dbname)));
	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, disk_quota_sighup);
	pqsignal(SIGTERM, disk_quota_sigterm);
	pqsignal(SIGUSR1, disk_quota_sigusr1);

	if (!MyWorkerInfo->dbEntry->inited)
	{
		MyWorkerInfo->dbEntry->last_log_time = GetCurrentTimestamp();
		ereport(LOG, (errmsg("[diskquota] start disk quota worker process to monitor database:%s", dbname)));
	}
	/* To avoid last_log_time from being uninitialized. */
	if (MyWorkerInfo->dbEntry->last_log_time > GetCurrentTimestamp())
		MyWorkerInfo->dbEntry->last_log_time = GetCurrentTimestamp();
	/*
	 * The shmem exit hook is registered after registering disk_quota_sigterm.
	 * So if the SIGTERM arrives before this statement, the shmem exit hook
	 * won't be called.
	 *
	 * TODO: launcher to free the unused worker?
	 */
	on_shmem_exit(FreeWorkerOnExit, 0);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

#if GP_VERSION_NUM < 70000
	/* Connect to our database */
	BackgroundWorkerInitializeConnection(dbname, NULL);
	set_config_option("application_name", DISKQUOTA_APPLICATION_NAME, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true,
	                  0);
#else
	BackgroundWorkerInitializeConnection(dbname, NULL, 0);
	set_config_option("application_name", DISKQUOTA_APPLICATION_NAME, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true,
	                  0, true);
#endif /* GP_VERSION_NUM */

	/* diskquota worker should has Gp_role as dispatcher */
	Gp_role = GP_ROLE_DISPATCH;

	/*
	 * Initialize diskquota related local hash map and refresh model
	 * immediately
	 */
	init_disk_quota_model(MyWorkerInfo->dbEntry->id);

	// FIXME: version check should be run for each starting bgworker?
	//  check current binary version and SQL DLL version are matched
	int times = 0;
	while (!got_sigterm)
	{
		CHECK_FOR_INTERRUPTS();

		int major = -1, minor = -1;
		int has_error = worker_spi_get_extension_version(&major, &minor) != 0;

		if (major == DISKQUOTA_MAJOR_VERSION && minor == DISKQUOTA_MINOR_VERSION) break;
#if GP_VERSION_NUM < 70000
		/* MemoryAccount has been removed on gpdb7 */
		MemoryAccounting_Reset();
#endif /* GP_VERSION_NUM */
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

		int rc = DiskquotaWaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
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

	/* suppose the database is ready, if not, then set it to false */
	bool is_ready = true;
	/* Waiting for diskquota state become ready */
	while (!got_sigterm)
	{
		int rc;
		/* If the database has been inited before, no need to check the ready state again */
		if (MyWorkerInfo->dbEntry->inited) break;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Check whether the state is in ready mode. The state would be
		 * unknown, when you `create extension diskquota` at the first time.
		 * After running UDF init_table_size_table() The state will changed to
		 * be ready.
		 */
		if (check_diskquota_state_is_ready())
		{
			is_ready = true;
			break;
		}

#if GP_VERSION_NUM < 70000
		MemoryAccounting_Reset();
#endif /* GP_VERSION_NUM */
		if (is_ready)
		{
			update_monitordb_status(MyWorkerInfo->dbEntry->dbid, DB_UNREADY);
			is_ready = false;
		}
		rc = DiskquotaWaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
		                        diskquota_naptime * 1000L);
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
	}

	if (!MyWorkerInfo->dbEntry->inited) update_monitordb_status(MyWorkerInfo->dbEntry->dbid, DB_RUNNING);

	bool        is_gang_destroyed    = false;
	TimestampTz loop_start_timestamp = 0;
	TimestampTz loop_end_timestamp;
	TimestampTz log_time;
	long        sleep_time = diskquota_naptime * 1000;
	long        secs;
	int         usecs;

	while (!got_sigterm)
	{
		int rc;

		/*
		 * The log printed from the bgworker does not contain the database name
		 * but contains the bgworker's pid. We should print the database name
		 * every BGWORKER_LOG_TIME to ensure that we can find the database name
		 * by the bgworker's pid in the log file.
		 */
		log_time = GetCurrentTimestamp();
		if (TimestampDifferenceExceeds(MyWorkerInfo->dbEntry->last_log_time, log_time, BGWORKER_LOG_TIME))
		{
			ereport(LOG, (errmsg("[diskquota] disk quota worker process is monitoring database:%s", dbname)));
			MyWorkerInfo->dbEntry->last_log_time = log_time;
		}

		/*
		 * If the bgworker receives a signal, the latch will be set ahead of the diskquota.naptime.
		 * To avoid too frequent diskquota refresh caused by receiving the signal, we use
		 * loop_start_timestamp and loop_end_timestamp to maintain the elapsed time since the last
		 * diskquota refresh. If the latch is set ahead of diskquota.naptime,
		 * refresh_disk_quota_model() should be skipped.
		 */
		loop_end_timestamp = GetCurrentTimestamp();
		TimestampDifference(loop_start_timestamp, loop_end_timestamp, &secs, &usecs);
		sleep_time += secs * 1000 + usecs / 1000;
		if (sleep_time >= diskquota_naptime * 1000)
		{
			SIMPLE_FAULT_INJECTOR("diskquota_worker_main");
			if (!diskquota_is_paused())
			{
				/* Refresh quota model with init mode */
				refresh_disk_quota_model(!MyWorkerInfo->dbEntry->inited);
				MyWorkerInfo->dbEntry->inited = true;
				is_gang_destroyed             = false;
			}
			else if (!is_gang_destroyed)
			{
				DisconnectAndDestroyAllGangs(false);
				is_gang_destroyed = true;
			}
			worker_increase_epoch(MyWorkerInfo->dbEntry->dbid);

			// GPDB6 opend a MemoryAccount for us without asking us.
			// and GPDB6 did not release the MemoryAccount after SPI finish.
			// Reset the MemoryAccount although we never create it.
#if GP_VERSION_NUM < 70000
			MemoryAccounting_Reset();
#endif /* GP_VERSION_NUM */

			sleep_time = 0;
		}
		loop_start_timestamp = GetCurrentTimestamp();

		if (is_dynamic_mode())
		{
			break;
		}
		CHECK_FOR_INTERRUPTS();

		/*
		 * Background workers mustn't call usleep() or any direct equivalent:
		 * instead, they may wait on their process latch, which sleeps as
		 * necessary, but is awakened if postmaster dies.  That way the
		 * background process goes away immediately in an emergency.
		 */
		rc = DiskquotaWaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
		                        diskquota_naptime * 1000 - sleep_time);
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
	}

	if (got_sigterm)
		ereport(LOG, (errmsg("[diskquota] stop disk quota worker process to monitor database:%s", dbname)));
	ereport(DEBUG1, (errmsg("[diskquota] stop disk quota worker process to monitor database:%s", dbname)));
#if DISKQUOTA_DEBUG
	TimestampDifference(MyWorkerInfo->dbEntry->last_run_time, GetCurrentTimestamp(), &secs, &usecs);
	MyWorkerInfo->dbEntry->cost = secs * 1000L + usecs / 1000L;
#endif
	proc_exit(0);
}

static const char *
diskquota_status_check_soft_limit()
{
	// should run on coordinator only.
	Assert(IS_QUERY_DISPATCHER());

	bool           found, paused;
	MonitorDBEntry entry;
	LWLockAcquire(diskquota_locks.monitored_dbid_cache_lock, LW_SHARED);
	{
		entry  = hash_search(monitored_dbid_cache, &MyDatabaseId, HASH_FIND, &found);
		paused = found ? entry->paused : false;
	}
	LWLockRelease(diskquota_locks.monitored_dbid_cache_lock);

	// if worker no booted, aka 'CREATE EXTENSION' not called, diskquota is paused
	if (!found) return "paused";

	// if worker booted, check 'worker_map->is_paused'
	return paused ? "paused" : "on";
}

static const char *
diskquota_status_check_hard_limit()
{
	// should run on coordinator only.
	Assert(IS_QUERY_DISPATCHER());

	bool hardlimit = diskquota_hardlimit;

	bool paused = false;
	paused      = diskquota_is_paused();
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
	static char ret_version[64];
	int         ret = SPI_connect();
	Assert(ret = SPI_OK_CONNECT);

	ret = SPI_execute("select extversion from pg_extension where extname = 'diskquota'", true, 0);

	if (ret != SPI_OK_SELECT || SPI_processed != 1)
	{
		ereport(WARNING,
		        (errmsg("[diskquota] when reading installed version lines %ld code = %d", SPI_processed, ret)));
		goto fail;
	}

	if (SPI_processed == 0)
	{
		goto fail;
	}

	bool  is_null       = false;
	Datum version_datum = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &is_null);
	Assert(is_null == false);

	char *version = TextDatumGetCString(version_datum);
	if (version == NULL || *version == '\0')
	{
		ereport(WARNING, (errmsg("[diskquota] 'extversion' is empty in pg_class.pg_extension. may catalog corrupted")));
		goto fail;
	}

	StrNCpy(ret_version, version, sizeof(ret_version) - 1);

	SPI_finish();
	return ret_version;

fail:
	SPI_finish();
	return "";
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
		TupleDesc tupdesc;
		funcctx = SRF_FIRSTCALL_INIT();

		MemoryContext oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		{
			tupdesc = DiskquotaCreateTemplateTupleDesc(2);
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

static void
FreeWorkerOnExit(int code, Datum arg)
{
	if (MyWorkerInfo != NULL)
	{
		FreeWorker(MyWorkerInfo);
	}
}

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
#include "msg_looper.h"
#include "diskquota_center_worker.h"

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

DiskQuotaLocks diskquota_locks;

/* functions of disk quota*/
void _PG_init(void);
void _PG_fini(void);

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
	BackgroundWorker center_worker;
	memset(&worker, 0, sizeof(BackgroundWorker));
	memset(&center_worker, 0, sizeof(BackgroundWorker));

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

	/* set up common data for diskquota center worker */
	center_worker.bgw_flags      = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	center_worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	/* center worker process should be restarted after pm reset. */
	center_worker.bgw_restart_time = BGW_DEFAULT_RESTART_INTERVAL;
	snprintf(center_worker.bgw_library_name, BGW_MAXLEN, DISKQUOTA_BINARY_NAME);
	snprintf(center_worker.bgw_function_name, BGW_MAXLEN, "disk_quota_center_worker_main");
	center_worker.bgw_notify_pid = 0;

	snprintf(center_worker.bgw_name, BGW_MAXLEN, "[diskquota] - center worker");

	RegisterBackgroundWorker(&center_worker);
}

void
_PG_fini(void)
{}

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

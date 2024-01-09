/*-------------------------------------------------------------------------
 *
 * diskquota_bgworker.c
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/diskquota_bgworker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "cdb/cdbgang.h"
#include "cdb/cdbvars.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/utility.h"
#include "utils/ps_status.h"
#include "utils/syscache.h"

#include "diskquota.h"
#include "msg_looper.h"
#include "diskquota_bgworker.h"
#include "diskquota_center_worker.h"
#include "diskquota_launcher.h"
#include "diskquota_guc.h"
#include "message_def.h"
#include "relation_cache.h"
#include "table_size.h"
#include "gp_activetable.h"

#include <unistd.h> // for useconds_t

/* sinal callback function */
static void disk_quota_sigterm(SIGNAL_ARGS);
static void disk_quota_sighup(SIGNAL_ARGS);

/* bgworker main function */
void        disk_quota_worker_main_3(Datum main_arg);
static void disk_quota_refresh(bool is_init);

/* extern function */
// FIXME: free worker on launcher
static void FreeWorkerOnExit(int code, Datum arg);
extern bool is_dynamic_mode(void);
extern int  usleep(useconds_t usec); // in <unistd.h>

/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup  = false;
static volatile sig_atomic_t got_sigterm = false;

/*
 * A pointer to DiskquotaLauncherShmem->workerEntries in shared memory
 * Only access in diskquota worker, different from each worker.
 */
static DiskQuotaWorkerEntry *volatile MyWorkerInfo = NULL;

/*---------------------------------signal callback function--------------------------------------------*/
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

/* ---- Functions for disk quota worker process ---- */
static void
FreeWorkerOnExit(int code, Datum arg)
{
	if (MyWorkerInfo != NULL)
	{
		FreeWorker(MyWorkerInfo);
	}
}

/*
 * Disk quota worker process will refresh disk quota model periodically.
 * Refresh logic is defined in quotamodel.c
 */
void
disk_quota_worker_main_3(Datum main_arg)
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
	// FIXME: Do we need disk_quota_sigusr1 in bgworker?
	// pqsignal(SIGUSR1, disk_quota_sigusr1);

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

	char bgworker_resource_owner[NAMEDATALEN * 2];
	sprintf(bgworker_resource_owner, "diskquota_bgworker %s", dbname);
	CurrentResourceOwner = ResourceOwnerCreate(NULL, bgworker_resource_owner);

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
		 * disk_quota_refresh() should be skipped.
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
				disk_quota_refresh(!MyWorkerInfo->dbEntry->inited);
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
		                        diskquota_naptime * 1000L - sleep_time);
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

// TODO: fetch table size from segments when init
static void
disk_quota_refresh(bool is_init)
{
	List                   *oidlist;
	HTAB                   *local_active_table_map;
	HTAB                   *local_table_size_map;
	DiskquotaLooper        *looper;
	DiskquotaMessage       *req_msg;
	DiskquotaMessage       *rsp_msg;
	Size                    msg_sz = 0;
	ReqMsgRefreshTableSize *req_msg_body;
	int                     oid_list_len;
	int                     table_size_entry_list_len;

	// TODO: do not load table size from diskquota.table_size
	// when is_init == true
	/* get active table size from all segments */
	local_active_table_map = gp_fetch_active_tables(is_init);
	/* get all relation oids in the current database */
	oidlist = get_current_database_oid_list();
	/* get table size map by local_active_table_map */
	local_table_size_map = get_current_database_table_size_map(local_active_table_map);

	oid_list_len              = list_length(oidlist);
	table_size_entry_list_len = hash_get_num_entries(local_table_size_map);

	/*
	 * message content:
	 * - ReqMsgRefreshTableSize
	 * - oid list
	 * - TableSizeEntry list
	 */
	msg_sz = add_size(msg_sz, sizeof(ReqMsgRefreshTableSize));
	msg_sz = add_size(msg_sz, oid_list_len * sizeof(Oid));
	msg_sz = add_size(msg_sz, table_size_entry_list_len * sizeof(TableSizeEntry));

	/* attach the meesage looper */
	looper = attach_message_looper(DISKQUOTA_CENTER_WORKER_MESSAGE_LOOPER_NAME);
	/* initialize request message */
	req_msg = InitRequestMessage(MSG_REFRESH_TABLE_SIZE, msg_sz);
	/* get message body */
	req_msg_body = (ReqMsgRefreshTableSize *)MessageBody(req_msg);

	/* fill message content in meesage body */
	req_msg_body->dbid                         = MyDatabaseId;
	req_msg_body->segcount                     = SEGCOUNT;
	req_msg_body->oid_list_len                 = oid_list_len;
	req_msg_body->table_size_entry_list_len    = table_size_entry_list_len;
	req_msg_body->oid_list_offset              = sizeof(ReqMsgRefreshTableSize);
	req_msg_body->table_size_entry_list_offset = req_msg_body->oid_list_offset + oid_list_len * sizeof(Oid);
	fill_message_content_by_list(MessageContentListAddr(req_msg_body, req_msg_body->oid_list_offset), oidlist,
	                             sizeof(Oid));
	fill_message_content_by_hash_table(MessageContentListAddr(req_msg_body, req_msg_body->table_size_entry_list_offset),
	                                   local_table_size_map, sizeof(TableSizeEntry));

	/* send request message and wait for response message */
	// TODO: add signal handle function
	rsp_msg = send_request_and_wait(looper, req_msg, NULL);

	// TODO: handle response message
	// update reject map and dispatch to segments

	free_message(req_msg);
	free_message(rsp_msg);
	list_free(oidlist);
	hash_destroy(local_active_table_map);
	hash_destroy(local_table_size_map);
}

/*-------------------------------------------------------------------------
 *
 * diskquota_center_worker.c
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/diskquota_center_worker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "cdb/cdbvars.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/utility.h"
#include "utils/ps_status.h"
#include "utils/syscache.h"
#include "utils/resowner.h"

#include "diskquota.h"
#include "msg_looper.h"
#include "diskquota_center_worker.h"

/* sinal callback function */
static void disk_quota_sigterm(SIGNAL_ARGS);
static void disk_quota_sighup(SIGNAL_ARGS);

/* center worker main function */
void                     disk_quota_center_worker_main(Datum main_arg);
static void              loop(DiskquotaLooper *looper);
static DiskquotaMessage *disk_quota_message_handler(DiskquotaMessage *req_msg);

/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup  = false;
static volatile sig_atomic_t got_sigterm = false;

/*---------------------------------signal callback function--------------------------------------------*/
static void
disk_quota_sigterm(SIGNAL_ARGS)
{
	int save_errno = errno;

	got_sigterm = true;

	DiskquotaLooper *looper = attach_looper(DISKQUOTA_CENTER_WORKER_NAME);
	if (looper) SetLatch(looper->slatch);

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

	DiskquotaLooper *looper = attach_looper(DISKQUOTA_CENTER_WORKER_NAME);
	if (looper) SetLatch(looper->slatch);

	errno = save_errno;
}

/*-------------------------------init function-------------------------------------------*/
/* total shared memory size used by center worker */
Size
diskquota_center_worker_shmem_size(void)
{
	Size size = 0;
	size      = add_size(size, sizeof(DiskquotaLooper));
	return size;
}

/*--------------------------------bgworker main---------------------------------------*/
/*
 * Disk quota worker process will refresh disk quota model periodically.
 * Refresh logic is defined in quotamodel.c
 */
void
disk_quota_center_worker_main(Datum main_arg)
{
	/* the center worker should exit when the master boots in utility mode */
	if (Gp_role != GP_ROLE_DISPATCH)
	{
		proc_exit(0);
	}

	/* Disable ORCA to avoid fallback */
	optimizer = false;

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, disk_quota_sighup);
	pqsignal(SIGTERM, disk_quota_sigterm);

	ereport(LOG, (errmsg("[diskquota] center worker start")));

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

#if GP_VERSION_NUM < 70000
	/* Connect to our database */
	BackgroundWorkerInitializeConnection(DISKQUOTA_DB, NULL);
	set_config_option("application_name", DISKQUOTA_APPLICATION_NAME, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true,
	                  0);
#else
	BackgroundWorkerInitializeConnection(DISKQUOTA_DB, NULL, 0);
	set_config_option("application_name", DISKQUOTA_APPLICATION_NAME, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true,
	                  0, true);
#endif /* GP_VERSION_NUM */

	init_ps_display("center worker:", "[diskquota]", DISKQUOTA_DB, "");

	CurrentResourceOwner    = ResourceOwnerCreate(NULL, DISKQUOTA_CENTER_WORKER_NAME);
	DiskquotaLooper *looper = init_looper(DISKQUOTA_CENTER_WORKER_NAME, disk_quota_message_handler);
	Assert(looper != NULL);

	// FIXME: should we destroy gangs?
	while (!got_sigterm)
	{
		loop(looper);
	}

	if (got_sigterm) ereport(LOG, (errmsg("[diskquota] stop center worker")));
	ereport(DEBUG1, (errmsg("[diskquota] stop center worker")));
	// TODO: terminate_launcher();
	proc_exit(0);
}

/*
 * Message handle function on the center worker.
 * - center worker always waits for `slatch`.
 * - as soon as `slatch` is set, center worker gets a request message from client.
 * - handles the request.
 * - write the response message by `rsp_handle`.
 * - set `clatch` to notify client to handle the response message.
 */
static void
loop(DiskquotaLooper *looper)
{
	int rc = DiskquotaWaitLatch(looper->slatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, 0);
	ResetLatch(looper->slatch);

	/* Emergency bailout if postmaster has died */
	if (rc & WL_POSTMASTER_DEATH)
	{
		ereport(LOG, (errmsg("[diskquota] center worker is being terminated by postmaster death.")));
		proc_exit(1);
	}

	CHECK_FOR_INTERRUPTS();

	/* in case of a SIGHUP, just reload the configuration. */
	if (got_sighup)
	{
		elog(DEBUG1, "[diskquota] got sighup");
		got_sighup = false;
		ProcessConfigFile(PGC_SIGHUP);
	}

	if (got_sigterm) return;

	/* handle the message sent by bgworker */
	if (looper->request_done || looper->req_handle != DSM_HANDLE_INVALID)
	{
		DiskquotaMessage *req_msg = attach_message(looper->req_handle);

		/* free the response message generated by the last connection. */
		free_message_by_handle(looper->rsp_handle);
		looper->rsp_handle    = DSM_HANDLE_INVALID;
		looper->response_done = false;

		DiskquotaMessage *rsp_msg = looper->msg_handler(req_msg);
		looper->rsp_handle        = rsp_msg->handle;
		looper->response_done     = true;
		looper->request_done      = false;
		free_message(req_msg);

		SetLatch(looper->clatch);

		// GPDB6 opend a MemoryAccount for us without asking us.
		// and GPDB6 did not release the MemoryAccount after SPI finish.
		// Reset the MemoryAccount although we never create it.
#if GP_VERSION_NUM < 70000
		MemoryAccounting_Reset();
#endif /* GP_VERSION_NUM */
	}
}

static DiskquotaMessage *
disk_quota_message_handler(DiskquotaMessage *req_msg)
{
	switch (req_msg->msg_id)
	{
		case MSG_TestMessage: {
			DiskquotaMessage *rsp_msg = init_message(MSG_TestMessage, sizeof(TestMessage));
			memcpy(MSG_BODY(rsp_msg), MSG_BODY(req_msg), MSG_SIZE(sizeof(TestMessage)));
			return rsp_msg;
		}
		break;
		default:
			break;
	}
	return NULL;
}

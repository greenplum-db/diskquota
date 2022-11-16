/*-------------------------------------------------------------------------
 *
 * msg_looper.c
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/msg_looper.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/dsm.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "miscadmin.h"

#include "diskquota.h"
#include "msg_looper.h"

/*----------------------------init function-----------------------------*/
static void
init_looper_lock(DiskquotaLooper *looper)
{
	/* lock is reserved in init_center_worker() */
#if GP_VERSION_NUM < 70000
	looper->loop_lock = LWLockAssign();
#else
	LWLockPadded *lock_base = GetNamedLWLockTranche(DISKQUOTA_CENTER_WORKER_LOCK_TRANCHE_NAME);
	looper->loop_lock       = &lock_base[0].lock;
#endif /* GP_VERSION_NUM */
}

/* init message looper. This function is called in center worker main function */
DiskquotaLooper *
init_looper(const char *name, message_handler handler)
{
	bool found = false;

	DiskquotaLooper *looper = (DiskquotaLooper *)ShmemInitStruct(name, sizeof(DiskquotaLooper), &found);
	if (found)
	{
		// TODO Report warning
		return looper;
	}
	looper->server_pid = MyProcPid;
	looper->client_pid = InvalidPid;

	looper->slatch = &MyProc->procLatch;
	looper->clatch = NULL;

	/* Init locks */
	init_looper_lock(looper);

	looper->req_handle = DSM_HANDLE_INVALID;
	looper->rsp_handle = DSM_HANDLE_INVALID;

	looper->request_done  = false;
	looper->response_done = false;

	looper->msg_handler = handler;

	return looper;
}

/*
 * Create a message.
 */
DiskquotaMessage *
init_message(DiskquotaMessageID msg_id, size_t payload_len)
{
	dsm_segment      *seg = dsm_create(MSG_SIZE(payload_len));
	DiskquotaMessage *msg = dsm_segment_address(seg);
	msg->msg_id           = msg_id;
	msg->handle           = dsm_segment_handle(seg);
	msg->size             = payload_len;
	return msg;
}

DiskquotaMessage *
attach_message(dsm_handle handle)
{
	dsm_segment *seg = dsm_attach(handle);
	return dsm_segment_address(seg);
}

void
free_message(DiskquotaMessage *msg)
{
	dsm_detach(dsm_find_mapping(msg->handle));
}

/*
 * Called by server to free the response dsm created in the last connection.
 */
void
free_message_by_handle(dsm_handle handle)
{
	if (handle == DSM_HANDLE_INVALID) return;
	dsm_detach(dsm_find_mapping(handle));
}

/*------------------------------client---------------------------------*/
/*
 * Attach to the message looper.
 * If the looper is not initialized yet, the return value is NULL.
 */
DiskquotaLooper *
attach_looper(const char *name)
{
	bool found = false;
	// FIXME: When to de-init?
	DiskquotaLooper *looper = (DiskquotaLooper *)ShmemInitStruct(name, sizeof(DiskquotaLooper), &found);
	if (!found)
	{
		/*
		 * The looper has not been started yet. Even the shmem can be created,
		 * the looper's client cannot do anything with it.
		 */
		return NULL;
	}
	// TODO: Check pid invalid
	return looper;
}

/*
 * send a request to server and wait for response
 * request segment and response segment should be free by client
 */
DiskquotaMessage *
send_request_and_wait(DiskquotaLooper *looper, DiskquotaMessage *req_msg, signal_handler handler)
{
	LWLockAcquire(looper->loop_lock, LW_EXCLUSIVE);

	/* own the client latch */
	looper->client_pid = MyProcPid;
	looper->clatch     = &MyProc->procLatch;
	looper->req_handle = req_msg->handle;
	/*
	 * reset response_done to avoid this scene:
	 * - response_done is set to true by the last connect
	 * - client gets a signal
	 * - client finish the waiting loop but does not get reponse
	 */
	looper->response_done = false;
	/*
	 * set request_done to true instead of use LWLock: in the center worker
	 * - if request_done is false due to the parallel problem, the center worker can check whether
	 *   req_handle != DSM_HANDLE_INVALID.
	 * 		- if YES, the center worker can go on to handle this message.
	 * 		- if NO, the center worker will handle this message in the next loop. Because SetLatch(looper->slatch) is
	 * 		  behind of ResetLatch(looper->slatch), the center worker won't hang by DiskquotaWaitLatch() in the next
	 *        loop.
	 * - if request_done is true, then the request message is set completely.
	 */
	looper->request_done = true;
	SetLatch(looper->slatch);

	do
	{
		/* Wait for response from server */
		int rc = DiskquotaWaitLatch(looper->clatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, 0);
		// FIXME: handle rc
		if (rc & WL_POSTMASTER_DEATH)
		{
			// FIXME: free resource
			ereport(ERROR, (errmsg("loop death, caused by postmaster death")));
		}
		ResetLatch(looper->clatch);

		if (handler != NULL) handler();
	} while (looper->response_done == false);

	DiskquotaMessage *rsp_msg = attach_message(looper->rsp_handle);

	looper->req_handle    = DSM_HANDLE_INVALID;
	looper->client_pid    = InvalidPid;
	looper->clatch        = NULL;
	looper->response_done = false;
	looper->request_done  = false;

	LWLockRelease(looper->loop_lock);

	return rsp_msg;
}

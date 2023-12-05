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

#include "pgstat.h"
#include "storage/dsm.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/ipc.h"
#include "miscadmin.h"

#include "msg_looper.h"

struct DiskquotaLooper
{
	pid_t server_pid; /* the pid of the server */
	pid_t client_pid; /* the oud if the client */

	Latch *slatch; /* the latch on the server side */
	Latch *clatch; /* the latch on the client side */

	LWLock *loop_lock; /* Clients holds this lock before sending request, and release it after receiving response. */

	dsm_handle req_handle; /* the dsm handle of request message */
	dsm_handle rsp_handle; /* the dsm handle of response message */

	bool request_done; /* Check whether request message is prepared. */
	/*
	 * Check whether response message is generated. Client waits for latch after send request,
	 * then if client receives a signal, the latch will be set and client will go on.
	 * In this case, client should check whether response_done == true, if false, client should
	 * wait for the latch again.
	 */
	bool response_done;

	/* Private to server */
	NameData        name;
	message_handler msg_handler;
};

/*----------------------------init function-----------------------------*/
Size
message_looper_size(void)
{
	return sizeof(DiskquotaLooper);
}

void
request_message_looper_lock(const char *looper_name)
{
#if GP_VERSION_NUM < 70000
	RequestAddinLWLocks(1);
#else
	RequestNamedLWLockTranche(looper_name, 1);
#endif /* GP_VERSION_NUM */
}

static void
init_looper_lock(DiskquotaLooper *looper)
{
	/* lock is reserved in init_center_worker() */
#if GP_VERSION_NUM < 70000
	looper->loop_lock = LWLockAssign();
#else
	LWLockPadded *lock_base = GetNamedLWLockTranche(NameStr(looper->name));
	looper->loop_lock       = &lock_base[0].lock;
#endif /* GP_VERSION_NUM */
}

void
init_message_looper(DiskquotaLooper *looper, message_handler handler)
{
	looper->server_pid  = MyProcPid;
	looper->slatch      = &(MyProc->procLatch);
	looper->msg_handler = handler;
	init_looper_lock(looper);
}

/* init message looper. This function is called in server main function */
DiskquotaLooper *
create_message_looper(const char *looper_name)
{
	bool found = false;

	DiskquotaLooper *looper = (DiskquotaLooper *)ShmemInitStruct(looper_name, sizeof(DiskquotaLooper), &found);
	if (found)
	{
		// TODO Report warning
		return looper;
	}

	Assert(strlen(looper_name) < NAMEDATALEN);
	StrNCpy(looper->name.data, looper_name, NAMEDATALEN);

	looper->server_pid = InvalidPid;
	looper->client_pid = InvalidPid;

	looper->slatch = NULL;
	looper->clatch = NULL;

	looper->req_handle = DSM_HANDLE_INVALID;
	looper->rsp_handle = DSM_HANDLE_INVALID;

	looper->request_done  = false;
	looper->response_done = false;

	return looper;
}

/*--------------------------message----------------------------------*/
/*
 * Create a message.
 */
DiskquotaMessage *
init_message(DiskquotaMessageID msg_id, size_t payload_len)
{
#if GP_VERSION_NUM < 70000
	dsm_segment *seg = dsm_create(MSG_SIZE(payload_len));
#else
	dsm_segment *seg        = dsm_create(MSG_SIZE(payload_len), 0);
#endif
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

/*------------------------------server---------------------------------*/
void
message_looper_set_server_latch(DiskquotaLooper *looper)
{
	SetLatch(looper->slatch);
}

void
message_looper_wait_for_latch(DiskquotaLooper *looper)
{
	/* Wait for request from client */
	int rc;
#if GP_VERSION_NUM < 70000
	rc = WaitLatch(looper->slatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, 0);
#else

	rc = WaitLatch(looper->slatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, 0, WAIT_EVENT_PG_SLEEP);
#endif
	ResetLatch(looper->slatch);

	/* Emergency bailout if postmaster has died */
	if (rc & WL_POSTMASTER_DEATH)
	{
		ereport(LOG, (errmsg("message looper server is being terminated by postmaster death.")));
		proc_exit(1);
	}
}

void
message_looper_handle_message(DiskquotaLooper *looper)
{
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

/*------------------------------client---------------------------------*/
/*
 * Attach to the message looper.
 * If the looper is not initialized yet, the return value is NULL.
 */
DiskquotaLooper *
attach_message_looper(const char *name)
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

static void
looper_wait_for_client_latch(DiskquotaLooper *looper)
{
	/* Wait for response from server */
	int rc;
#if GP_VERSION_NUM < 70000
	rc = WaitLatch(looper->clatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, 0);
#else
	rc = WaitLatch(looper->clatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, 0, WAIT_EVENT_PG_SLEEP);
#endif

	// FIXME: handle rc
	if (rc & WL_POSTMASTER_DEATH)
	{
		// FIXME: free resource
		ereport(ERROR, (errmsg("loop death, caused by postmaster death")));
	}
	ResetLatch(looper->clatch);
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
	 * set request_done to true instead of use LWLock: in the server
	 * - if request_done is false due to the parallel problem, the server can check whether
	 *   req_handle != DSM_HANDLE_INVALID.
	 * 		- if YES, the server can go on to handle this message.
	 * 		- if NO, the server will handle this message in the next loop. Because SetLatch(looper->slatch) is
	 * 		  behind of ResetLatch(looper->slatch), the server won't hang by DiskquotaWaitLatch() in the next
	 *        loop.
	 * - if request_done is true, then the request message is set completely.
	 */
	looper->request_done = true;
	SetLatch(looper->slatch);

	do
	{
		looper_wait_for_client_latch(looper);
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

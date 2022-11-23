#include "postgres.h"
#include "storage/dsm.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include <miscadmin.h>
#include "utils/timeout.h"

#include "msg.h"

void init_timeout(void);

typedef struct DsmMessage
{
	int   msg_id;
	void *payload;
} DsmMessage;

#define REQ_TEST 0
typedef struct
{
	int id;
	struct
	{
		int content;
	} Req;
	struct
	{
		int bool;
	} Resp;
} TestMessage;

DsmLooper *
init_looper(const char *name, message_handler handler)
{
	bool found = false;

	DsmLooper *looper = (DsmLooper *)ShmemInitStruct(name, sizeof(DsmLooper), &found);
	if (found)
	{
		// TODO Report warning
	}
	looper->server_pid = MyProcPid;
	looper->client_pid = InvalidPid;

	looper->server_latch = MyProc->procLatch;
	InitSharedLatch(&looper->server_latch);
	OwnLatch(&looper->server_latch);
	InitSharedLatch(&looper->client_latch);

	// Init locks
	RequestAddinLWLocks(2);
	looper->loop_lock = LWLockAssign();
	looper->msg_lock  = LWLockAssign();

	looper->msg_handler = handler;

	return looper;
}

DsmLooper *
attach_looper(const char *name)
{
	bool found = false;
	// FIXME: When to de-init?
	DsmLooper *looper = (DsmLooper *)ShmemInitStruct(name, sizeof(DsmLooper), &found);
	if (!found)
	{
		// The looper has not been started yet. Even the shmem can be created, the looper's client
		// cannot do anything with it.
		return NULL;
	}
	// TODO: Check pid invalid
	return looper;
}

#if 0
ShmMessage*
alloc_message(size_t msg_len){
	dsm_segment *seg = dsm_create(msg_len);
	ShmMessage *msg = dsm_segment_address(seg);
	msg->msg_seg = seg;
	msg->payload = &msg->payload;
	return msg;
}
#endif

dsm_segment *
init_message(int msg_id, size_t payload_len)
{
	dsm_segment *seg = dsm_create(sizeof(DsmMessage) + payload_len);
	DsmMessage  *msg = dsm_segment_address(seg);
	msg->msg_id      = msg_id;
	msg->payload     = msg + sizeof(DsmMessage);
	return seg;
}

int
get_msg_id(dsm_segment *dsm_seg)
{
	DsmMessage *msg = dsm_segment_address(dsm_seg);
	return msg->msg_id;
}

void *
get_payload_ptr(dsm_segment *dsm_seg)
{
	DsmMessage *msg = dsm_segment_address(dsm_seg);
	return msg->payload;
}

dsm_segment *
send_request_and_wait(DsmLooper *looper, dsm_segment *req_seg)
{
	LWLockAcquire(looper->loop_lock, LW_EXCLUSIVE);

	LWLockAcquire(looper->msg_lock, LW_EXCLUSIVE);

	looper->req_handle = dsm_segment_handle(req_seg);
	looper->client_pid = MyProcPid;

	OwnLatch(&looper->client_latch);
	SetLatch(&looper->server_latch);
	LWLockRelease(looper->msg_lock);

	// Wait for response
	int rc = WaitLatch(&looper->client_latch, WL_LATCH_SET | WL_POSTMASTER_DEATH, 0);
	// FIXME: handle rc
	if (rc & WL_POSTMASTER_DEATH)
	{
		// FIXME: free resource
		ereport(ERROR, (errmsg("loop death, caused by postmaster death")));
	}
	ResetLatch(&looper->client_latch);

	dsm_segment *rsp_seg = dsm_attach(looper->rsp_handle);
	// FIXME: Do we need to call dsm_detach on req_handle?
	looper->req_handle = DSM_HANDLE_INVALID;
	looper->rsp_handle = DSM_HANDLE_INVALID;
	looper->client_pid = InvalidPid;
	DisownLatch(&looper->client_latch);

	LWLockRelease(looper->loop_lock);

	return rsp_seg;
}

void
loop(DsmLooper *looper)
{
	for (;;)
	{
		int rc = WaitLatch(&looper->server_latch, WL_LATCH_SET | WL_POSTMASTER_DEATH, 0);
		ResetLatch(&looper->server_latch);
		if (rc & WL_POSTMASTER_DEATH)
		{
			// TODO: Better message
			ereport(LOG, (errmsg("loop death, caused by postmaster death")));
			break;
		}

		if (handle_signal()) continue;

		LWLockAcquire(looper->msg_lock, LW_EXCLUSIVE);

		dsm_segment *req_seg = dsm_attach(looper->req_handle);
		DsmMessage  *msg     = dsm_segment_address(req_seg);
		dsm_segment *rsp_seg = looper->msg_handler(msg->msg_id, msg->payload);
		looper->rsp_handle   = dsm_segment_handle(rsp_seg);
		// FIXME: probably we need detach rsp_seg to avoid leaks
		dsm_detach(req_seg);
		SetLatch(&looper->client_latch);

		LWLockRelease(looper->msg_lock);
	}
}

typedef struct msg_a
{
	int a;
	int b;
} msg_a;

typedef struct msg_b
{
	int a;
	int b;
	int c;
} msg_b;

PG_FUNCTION_INFO_V1(test_send_a);
PG_FUNCTION_INFO_V1(test_send_b);
Datum
test_send_a(PG_FUNCTION_ARGS)
{
	DsmLooper   *looper  = attach_looper("diskquota_looper");
	dsm_segment *req_seg = init_message(42, sizeof(msg_a));
	msg_a       *body    = get_payload_ptr(req_seg);
	body->a              = 100;
	body->b              = 120;
	dsm_segment *rsp_seg = send_request_and_wait(looper, req_seg);
	int          id      = get_msg_id(rsp_seg);

	ereport(NOTICE, (errmsg("reponse id %d", id)));

	PG_RETURN_VOID();
}

Datum
test_send_b(PG_FUNCTION_ARGS)
{
	DsmLooper   *looper  = attach_looper("diskquota_looper");
	dsm_segment *req_seg = init_message(43, sizeof(msg_b));
	msg_b       *body    = get_payload_ptr(req_seg);
	body->a              = 100;
	body->b              = 120;
	body->c              = 1200000;
	dsm_segment *rsp_seg = send_request_and_wait(looper, req_seg);

	int id = get_msg_id(rsp_seg);
	ereport(NOTICE, (errmsg("reponse id %d", id)));

	PG_RETURN_VOID();
}

dsm_segment *
diskquota_message_handler(int message_id, void *req)
{
	switch (message_id)
	{
		// case TIMEOUT_EVENT: {

		// 	return init_message(TIMEOUT_EVENT, sizeof(DsmMessage));
		// }
		case 41: {
			msg_a *msg = (msg_a *)req;
			ereport(WARNING, (errmsg("request %d", msg->b)));
			return init_message(410, sizeof(DsmMessage));
		}
		case 42:
		default: {
			msg_b *msg = (msg_b *)req;
			ereport(WARNING, (errmsg("request %d", msg->c)));
			return init_message(410, sizeof(DsmMessage));
		}
	}
}

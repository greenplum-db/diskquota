#include "msg.h"

#include <storage/latch.h>
#include <storage/proc.h>
#include <storage/shmem.h>
#include <miscadmin.h>

struct DsmLooper {
	pid_t server_pid;
	pid_t client_pid;

	// Clients holds this lock before sending request, and release it after receiving response
	LWLock *loop_lock;
	// Server/Clients hold this lock when reading/writing messages
	LWLock *msg_lock;

	dsm_handle req_handle;
	dsm_handle rsp_handle;
} ;

#define REQ_TEST 0
typedef struct {
	int id;
	struct {
		int content;
	} Req;
	struct {
		int bool;
	} Resp;
} TestMessage;


DsmLooper*
init_looper(const char* name) {
	bool found = false;

	DsmLooper* looper = (DsmLooper*)ShmemInitStruct(name, sizeof(DsmLooper), &found);
	if (found) {
		// TODO Report warning
	}
	looper->server_pid = MyProcPid;
	looper->client_pid = InvalidPid;

	// Init locks
	RequestAddinLWLocks(2);
	looper->loop_lock = LWLockAssign();
	looper->msg_lock = LWLockAssign();

	return looper;
}

DsmLooper*
attach_looper(const char* name)
{
	bool found = false;
	// FIXME: When to de-init?
	DsmLooper* looper = (DsmLooper*)ShmemInitStruct(name, sizeof(DsmLooper), &found);
	if (!found) {
		// The looper has not been started yet. Even the shmem can be created, the looper's client
		// cannot do anything with it.
		return NULL;
	}
	// TODO: Check pid invalid
	return looper;

}

ShmMessage*
alloc_message(size_t msg_len){
	dsm_segment *seg = dsm_create(msg_len);
	ShmMessage *msg = dsm_segment_address(seg);
	msg->msg_seg = seg;
	msg->payload = &msg->payload;
	return msg;
}

void
send_message(DsmLooper* looper, const ShmMessage* msg)
{
	LWLockAcquire(looper->loop_lock, LW_EXCLUSIVE);

	LWLockAcquire(looper->msg_lock, LW_EXCLUSIVE);

	looper->req_handle = dsm_segment_handle(msg->msg_seg);
	looper->client_pid = MyProcPid;

	int rc = kill(looper->server_pid, SIGUSR2);
	LWLockRelease(looper->msg_lock);

	// Wait for response
	rc = WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, 0);
	ResetLatch(&MyProc->procLatch);

	LWLockRelease(looper->loop_lock);
}

ShmMessage* receive_message(const DsmLooper* looper)
{
	int rc = WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, 0);
	ResetLatch(&MyProc->procLatch);
	dsm_segment* seg = dsm_attach(looper->req_handle);
	ShmMessage *msg = dsm_segment_address(seg);
	return msg;
}

PG_FUNCTION_INFO_V1(test_send);
Datum
test_send(PG_FUNCTION_ARGS)
{
	DsmLooper *looper = attach_looper("diskquota_looper");
	ShmMessage *msg = alloc_message(10);
	msg->message_id = 42;
	send_message(looper, msg);
	PG_RETURN_VOID();
}

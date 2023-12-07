#include "postgres.h"

#include "utils/faultinjector.h"
#include "catalog/pg_type.h"
#include "storage/ipc.h"
#include "funcapi.h"

#include "diskquota.h"
#include "msg_looper.h"
#include "diskquota_center_worker.h"
#include "message_def.h"

static void
disk_quota_sigterm(SIGNAL_ARGS)
{
	proc_exit(0);
}

/*---------------------------test UDF---------------------------------*/
PG_FUNCTION_INFO_V1(test_send_message);
Datum
test_send_message(PG_FUNCTION_ARGS)
{
	int a = PG_GETARG_INT32(0);
	int b = PG_GETARG_INT32(1);

	TupleDesc tupdesc = DiskquotaCreateTemplateTupleDesc(2);
	TupleDescInitEntry(tupdesc, 1, "a", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, 2, "b", INT4OID, -1, 0);
	TupleDesc tuple_desc = BlessTupleDesc(tupdesc);

	DiskquotaLooper  *looper  = attach_message_looper(DISKQUOTA_CENTER_WORKER_MESSAGE_LOOPER_NAME);
	DiskquotaMessage *req_msg = init_request_message(MSG_TestMessage, sizeof(TestMessage));
	DiskquotaMessage *rsp_msg;
	TestMessage      *body = (TestMessage *)MSG_BODY(req_msg);
	body->a                = a;
	body->b                = b;

	SIMPLE_FAULT_INJECTOR("diskquota_message_send");
	rsp_msg               = send_request_and_wait(looper, req_msg, NULL);
	TestMessage *msg_body = (TestMessage *)MSG_BODY(rsp_msg);

	bool      nulls[2] = {false, false};
	Datum     v[2]     = {Int32GetDatum(msg_body->a), Int32GetDatum(msg_body->b)};
	HeapTuple tuple    = heap_form_tuple(tuple_desc, v, nulls);
	Datum     result   = HeapTupleGetDatum(tuple);

	free_message(req_msg);
	free_message(rsp_msg);
	PG_RETURN_DATUM(result);
}

PG_FUNCTION_INFO_V1(test_send_message_loop);
Datum
test_send_message_loop(PG_FUNCTION_ARGS)
{
	int a = PG_GETARG_INT32(0);
	pqsignal(SIGTERM, disk_quota_sigterm);

	DiskquotaLooper *looper = attach_message_looper(DISKQUOTA_CENTER_WORKER_MESSAGE_LOOPER_NAME);

	while (true)
	{
		DiskquotaMessage *req_msg  = init_request_message(MSG_TestMessageLoop, sizeof(TestMessageLoop));
		TestMessageLoop  *req_body = (TestMessageLoop *)MSG_BODY(req_msg);
		req_body->a                = a;

		DiskquotaMessage *rsp_msg  = send_request_and_wait(looper, req_msg, NULL);
		TestMessageLoop  *rsp_body = (TestMessageLoop *)MSG_BODY(rsp_msg);
		Assert(rsp_body->a == req_body->a + 1);

		a++;
		free_message(req_msg);
		free_message(rsp_msg);
	}

	PG_RETURN_BOOL(true);
}

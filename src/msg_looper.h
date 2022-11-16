/*-------------------------------------------------------------------------
 *
 * msg_looper.h
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/msg_looper.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MSG_LOOPER_H
#define MSG_LOOPER_H

#include "postgres.h"
#include "storage/dsm.h"

#define MSG_BODY(msg) ((char *)msg + MAXALIGN(sizeof(DiskquotaMessage)))
#define MSG_SIZE(sz) (MAXALIGN(sizeof(DiskquotaMessage)) + MAXALIGN(sz))

typedef struct TestMessage
{
	int a;
	int b;
} TestMessage;

typedef enum
{
	MSG_DEBUG = 10000,
	MSG_TestMessage,
	TIMEOUT_EVENT,
} DiskquotaMessageID;

/*
 * The message header.
 * The message content follows the message header.,
 * we can call MSG_BODY() to get the address of the message content.
 */
typedef struct DiskquotaMessage
{
	DiskquotaMessageID msg_id; /* message ID */
	dsm_handle         handle; /* the dsm handle of the current message */
	Size               size;   /* the size of message content */
} DiskquotaMessage;

typedef DiskquotaMessage *(*message_handler)(DiskquotaMessage *req_msg);
typedef void (*signal_handler)(void);

typedef struct DiskquotaLooper
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

	/* Private to center worker */
	message_handler msg_handler;
} DiskquotaLooper;

/* Called by center worker */
extern DiskquotaLooper *init_looper(const char *name, message_handler handler);

/* Called by client */
extern DiskquotaLooper  *attach_looper(const char *name);
extern DiskquotaMessage *send_request_and_wait(DiskquotaLooper *looper, DiskquotaMessage *req_msg,
                                               signal_handler handler);

/* message function */
extern DiskquotaMessage *init_message(DiskquotaMessageID msg_id, size_t payload_len);
extern DiskquotaMessage *attach_message(dsm_handle handle);
extern void              free_message(DiskquotaMessage *msg);
extern void              free_message_by_handle(dsm_handle handle);

#endif

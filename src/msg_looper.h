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

#define DiskquotaMessageID uint32
#define MessageBody(msg) ((char *)msg + MAXALIGN(sizeof(DiskquotaMessage)))
#define MessageSize(msg) (MAXALIGN(msg->size) + MAXALIGN(sizeof(DiskquotaMessage)))
#define InitRequestMessage(msg_id, payload_len) init_message(msg_id, payload_len)
#define InitResponseMessage(msg_id, payload_len) init_message(msg_id, payload_len)
#define CopyValueFromMessageContentList(list, dst, idx, sz) \
	do                                                      \
	{                                                       \
		memcpy(dst, (char *)list + (idx) * (sz), sz);       \
	} while (0)

#define GetPointFromMessageContentList(list, idx, sz) ((char *)list + (idx) * (sz))

/*
 * The message header.
 * The message content follows the message header.,
 * we can call MessageBody() to get the address of the message content.
 */
typedef struct DiskquotaMessage
{
	DiskquotaMessageID msg_id; /* message ID */
	dsm_handle         handle; /* the dsm handle of the current message */
	Size               size;   /* the size of message content */
} DiskquotaMessage;

typedef struct DiskquotaLooper DiskquotaLooper;
typedef DiskquotaMessage *(*message_handler)(DiskquotaMessage *req_msg);
typedef void (*signal_handler)(void);

/* Called by server */
/*
 * To initialize a looper
 * 1. request shared memory in the _pg_init() on the postmaster process
 * by calling RequestAddinShmemSpace(message_looper_size())
 * 2. request locks in the _pg_init() on the postmaster process
 * request_message_looper_lock("my_looper");
 * 3. create the looper struct on the server process (cannot be the postmaster process)
 * looper = create_message_looper("my_looper");
 * 4. initialize the looper
 * init_message_looper(looper, handler);
 */
extern void             request_message_looper_lock(const char *looper_name);
extern DiskquotaLooper *create_message_looper(const char *looper_name);
extern void             init_message_looper(DiskquotaLooper *looper, message_handler handler);

extern Size message_looper_size(void);
extern void message_looper_wait_for_latch(DiskquotaLooper *looper);
extern void message_looper_handle_message(DiskquotaLooper *looper);
extern void message_looper_set_server_latch(DiskquotaLooper *looper);

/* Called by client */
extern DiskquotaLooper  *attach_message_looper(const char *name);
extern DiskquotaMessage *send_request_and_wait(DiskquotaLooper *looper, DiskquotaMessage *req_msg,
                                               signal_handler handler);

/* message function */
extern DiskquotaMessage *init_message(DiskquotaMessageID msg_id, size_t payload_len);
extern void              free_message(DiskquotaMessage *msg);
extern void              fill_message_content_by_list(void *addr, List *list, Size sz);
extern void              fill_message_content_by_hash_table(void *addr, HTAB *ht, Size sz);

#endif

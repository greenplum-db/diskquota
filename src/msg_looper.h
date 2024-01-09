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

/*
 * Example of message struct:
 * typedef struct ReqMsgSample {
 * 		int a;
 * 		int list1_len;
 * 		int list2_len;
 * 		size_t list1_offset;
 * 		size_t list2_offset;
 * } ReqMsgSample;
 *
 * - 'a' is one of the message content.
 * - 'list1_len' is the length of list1 in the message.
 * - 'list2_len' is the length of list2 in the message.
 * - 'list1_offset' is the address offset of list1.
 *   The address of list1 is behind of ReqMsgSample.
 * 	 list1_offset = sizeof(ReqMsgSample)
 * - 'list1_offset' is the address offset of list2.
 * 	 The address of list2 is behind of list1.
 * 	 list2_offset = list1_offset + list1_len * sizeof(list1_entry)
 */

#define DiskquotaMessageID uint32
#define MessageBody(msg) ((char *)msg + MAXALIGN(sizeof(DiskquotaMessage)))
#define MessageSize(msg) (MAXALIGN(msg->size) + MAXALIGN(sizeof(DiskquotaMessage)))
#define InitRequestMessage(msg_id, payload_len) init_message(msg_id, payload_len)
#define InitResponseMessage(msg_id, payload_len) init_message(msg_id, payload_len)
#define MessageContentListAddr(msg, offset) ((char *)(msg) + (offset))
#define CopyValueFromMessageContentList(msg, list_offset, dst, idx, sz)           \
	do                                                                            \
	{                                                                             \
		memcpy(dst, MessageContentListAddr(msg, list_offset) + (idx) * (sz), sz); \
	} while (0)

#define GetPointFromMessageContentList(msg, list_offset, idx, sz) \
	(MessageContentListAddr(msg, list_offset) + (idx) * (sz))

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

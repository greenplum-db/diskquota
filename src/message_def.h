/*-------------------------------------------------------------------------
 *
 * message_def.h
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/message_def.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MESSAGE_DEF_H
#define MESSAGE_DEF_H

#include "quota_config.h"

typedef struct TestMessage
{
	int a;
	int b;
} TestMessage;

typedef struct TestMessageLoop
{
	int a;
} TestMessageLoop;

#define MSG_DEBUG 1
#define MSG_TestMessage 2
#define MSG_TestMessageLoop 3
#define MSG_REFRESH_TABLE_SIZE 4
#define MSG_REFRESH_QUOTA_INFO 5
#define MSG_SET_QUOTA_CONFIG 6

typedef struct ReqMsgRefreshTableSize
{
	Oid    dbid;
	int    segcount;
	int    oid_list_len;
	int    table_size_entry_list_len;
	uint64 oid_list_offset;
	uint64 table_size_entry_list_offset;
} ReqMsgRefreshTableSize;

typedef struct ReqMsgSetQuotaConfig
{
	Oid         dbid;
	QuotaConfig config;
	bool        need_delete_quota;
} ReqMsgSetQuotaConfig;

typedef struct RspMsgSetQuotaConfig
{
	bool  success;
	char *error;
} RspMsgSetQuotaConfig;

#endif

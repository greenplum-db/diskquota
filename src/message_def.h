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
	size_t oid_list_offset;
	size_t table_size_entry_list_offset;
} ReqMsgRefreshTableSize;

typedef struct RspMsgRefreshTableSize
{
	int    quota_info_entry_list_len;
	size_t quota_info_entry_list_offset;
} RspMsgRefreshTableSize;

typedef struct ReqMsgRefreshQuotaInfo
{
	Oid    dbid;
	int    expired_quota_info_entry_list_len;
	size_t expired_quota_info_entry_list_offset;
} ReqMsgRefreshQuotaInfo;

typedef struct RspMsgRefreshQuotaInfo
{
	int    reject_map_entry_list_len;
	size_t reject_map_entry_list_offset;
} RspMsgRefreshQuotaInfo;

#endif

/*-------------------------------------------------------------------------
 *
 * ddl_message.h
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/ddl_message.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef DDL_MESSAGE_H
#define DDL_MESSAGE_H

#define report_ddl_err(ddl_msg, prefix)                                                      \
	do                                                                                       \
	{                                                                                        \
		MessageResult ddl_result_ = (MessageResult)ddl_msg->result;                          \
		const char   *ddl_err_;                                                              \
		const char   *ddl_hint_;                                                             \
		ddl_err_code_to_err_message(ddl_result_, &ddl_err_, &ddl_hint_);                     \
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s: %s", prefix, ddl_err_), \
		                ddl_hint_ ? errhint("%s", ddl_hint_) : 0));                          \
	} while (0)

typedef enum MessageResult
{
	ERR_PENDING = 0,
	ERR_OK,
	/* the number of database exceeds the maximum */
	ERR_EXCEED,
	/* add the dbid to diskquota_namespace.database_list failed */
	ERR_ADD_TO_DB,
	/* delete dbid from diskquota_namespace.database_list failed */
	ERR_DEL_FROM_DB,
	/* cann't start worker process */
	ERR_START_WORKER,
	/* invalid dbid */
	ERR_INVALID_DBID,
	ERR_UNKNOWN,
} MessageResult;

/*
 * MessageBox is used to store a message for communication between
 * the diskquota launcher process and backends.
 * When backend create an extension, it send a message to launcher
 * to start the diskquota worker process and write the corresponding
 *
 * dbOid into diskquota database_list table in postgres database.
 * When backend drop an extension, it will send a message to launcher
 * to stop the diskquota worker process and remove the dbOid from diskquota
 * database_list table as well.
 */
typedef struct ExtensionDDLMessage
{
	int launcher_pid; /* diskquota launcher pid */
	int req_pid;      /* pid of the QD process which create/drop
	                   * diskquota extension */
	int cmd;          /* message command type, see MessageCommand */
	int result;       /* message result writen by launcher, see
	                   * MessageResult */
	int dbid;         /* dbid of create/drop diskquota
	                   * extensionstatement */
} ExtensionDDLMessage;

extern ExtensionDDLMessage *extension_ddl_message;

extern void init_shm_ddl_message(void);
extern Size diskquota_ddl_message_shmem_size(void);

extern void ddl_err_code_to_err_message(MessageResult code, const char **err_msg, const char **hint_msg);

#endif

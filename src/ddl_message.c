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
#include "postgres.h"
#include "miscadmin.h"

#include "diskquota.h"
#include "ddl_message.h"

ExtensionDDLMessage *extension_ddl_message = NULL;

void
init_shm_ddl_message(void)
{
	bool found;
	extension_ddl_message = ShmemInitStruct("disk_quota_extension_ddl_message", sizeof(ExtensionDDLMessage), &found);
	if (!found) memset((void *)extension_ddl_message, 0, sizeof(ExtensionDDLMessage));
}

Size
diskquota_ddl_message_shmem_size(void)
{
	return sizeof(ExtensionDDLMessage);
}

/*
 * For extension DDL('create extension/drop extension')
 * Using this function to convert error code from diskquota
 * launcher to error message and return it to client.
 */
void
ddl_err_code_to_err_message(MessageResult code, const char **err_msg, const char **hint_msg)
{
	*hint_msg = NULL;
	switch (code)
	{
		case ERR_PENDING:
			*err_msg  = "no response from diskquota launcher, check whether launcher process exists";
			*hint_msg = "Create \"diskquota\" database and restart the cluster.";
			break;
		case ERR_OK:
			*err_msg = "succeeded";
			break;
		case ERR_EXCEED:
			*err_msg = "too many databases to monitor";
			break;
		case ERR_ADD_TO_DB:
			*err_msg = "add dbid to database_list failed";
			break;
		case ERR_DEL_FROM_DB:
			*err_msg = "delete dbid from database_list failed";
			break;
		case ERR_START_WORKER:
			*err_msg = "start diskquota worker failed";
			break;
		case ERR_INVALID_DBID:
			*err_msg = "invalid dbid";
			break;
		default:
			*err_msg = "unknown error";
			break;
	}
}

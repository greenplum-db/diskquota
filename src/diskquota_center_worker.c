/*-------------------------------------------------------------------------
 *
 * diskquota_center_worker.c
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/diskquota_center_worker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "cdb/cdbvars.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/utility.h"
#include "utils/ps_status.h"
#include "utils/syscache.h"
#include "utils/resowner.h"

#include "diskquota.h"
#include "msg_looper.h"
#include "diskquota_center_worker.h"
#include "message_def.h"
#include "table_size.h"
#include "quota.h"
#include "toc_map.h"
#include "rejectmap.h"

/* table of content map in center worker */
static HTAB *toc_map;

/* sinal callback function */
static void disk_quota_sigterm(SIGNAL_ARGS);
static void disk_quota_sighup(SIGNAL_ARGS);

/* center worker main function */
void                     disk_quota_center_worker_main(Datum main_arg);
static inline void       loop(DiskquotaLooper *looper);
static DiskquotaMessage *disk_quota_message_handler(DiskquotaMessage *req_msg);
static DiskquotaMessage *refresh_table_size(ReqMsgRefreshTableSize *req_msg_body);
static DiskquotaMessage *refresh_quota_info(ReqMsgRefreshQuotaInfo *req_msg_body);

/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup  = false;
static volatile sig_atomic_t got_sigterm = false;

/*---------------------------------signal callback function--------------------------------------------*/
static void
disk_quota_sigterm(SIGNAL_ARGS)
{
	int save_errno = errno;

	got_sigterm = true;

	DiskquotaLooper *looper = attach_message_looper(DISKQUOTA_CENTER_WORKER_MESSAGE_LOOPER_NAME);
	if (looper) message_looper_set_server_latch(looper);

	errno = save_errno;
}

/*
 * Signal handler for SIGHUP
 * Set a flag to tell the main loop to reread the config file, and set
 * our latch to wake it up.
 */
static void
disk_quota_sighup(SIGNAL_ARGS)
{
	int save_errno = errno;

	got_sighup = true;

	DiskquotaLooper *looper = attach_message_looper(DISKQUOTA_CENTER_WORKER_MESSAGE_LOOPER_NAME);
	if (looper) message_looper_set_server_latch(looper);

	errno = save_errno;
}

/*-------------------------------init function-------------------------------------------*/
/* total shared memory size used by center worker */
Size
diskquota_center_worker_shmem_size(void)
{
	Size size = 0;
	size      = add_size(size, message_looper_size());
	return size;
}

/*--------------------------------bgworker main---------------------------------------*/
/*
 * Disk quota worker process will refresh disk quota model periodically.
 * Refresh logic is defined in quotamodel.c
 */
void
disk_quota_center_worker_main(Datum main_arg)
{
	/* the center worker should exit when the master boots in utility mode */
	if (Gp_role != GP_ROLE_DISPATCH)
	{
		proc_exit(0);
	}

	/* Disable ORCA to avoid fallback */
	optimizer = false;

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, disk_quota_sighup);
	pqsignal(SIGTERM, disk_quota_sigterm);

	ereport(LOG, (errmsg("[diskquota] center worker start")));

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

#if GP_VERSION_NUM < 70000
	/* Connect to our database */
	BackgroundWorkerInitializeConnection(DISKQUOTA_DB, NULL);
	set_config_option("application_name", DISKQUOTA_APPLICATION_NAME, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true,
	                  0);
#else
	BackgroundWorkerInitializeConnection(DISKQUOTA_DB, NULL, 0);
	set_config_option("application_name", DISKQUOTA_APPLICATION_NAME, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true,
	                  0, true);
#endif /* GP_VERSION_NUM */

	init_ps_display("center worker:", "[diskquota]", DISKQUOTA_DB, "");

	CurrentResourceOwner    = ResourceOwnerCreate(NULL, DISKQUOTA_CENTER_WORKER_NAME);
	DiskquotaLooper *looper = create_message_looper(DISKQUOTA_CENTER_WORKER_MESSAGE_LOOPER_NAME);
	Assert(looper != NULL);
	init_message_looper(looper, disk_quota_message_handler);
	toc_map = init_toc_map();

	// FIXME: should we destroy gangs?
	while (!got_sigterm)
	{
		loop(looper);
	}

	if (got_sigterm) ereport(LOG, (errmsg("[diskquota] stop center worker")));
	ereport(DEBUG1, (errmsg("[diskquota] stop center worker")));
	// TODO: terminate_launcher();
	proc_exit(0);
}

/*
 * Message handle function on the center worker.
 * - center worker always waits for `slatch`.
 */
static inline void
loop(DiskquotaLooper *looper)
{
	message_looper_wait_for_latch(looper);

	CHECK_FOR_INTERRUPTS();

	/* in case of a SIGHUP, just reload the configuration. */
	if (got_sighup)
	{
		elog(DEBUG1, "[diskquota] got sighup");
		got_sighup = false;
		ProcessConfigFile(PGC_SIGHUP);
	}

	if (got_sigterm) return;

	message_looper_handle_message(looper);
}

static DiskquotaMessage *
disk_quota_message_handler(DiskquotaMessage *req_msg)
{
	DiskquotaMessage *rsp_msg = NULL;
	switch (req_msg->msg_id)
	{
		case MSG_TestMessage: {
			rsp_msg = InitResponseMessage(MSG_TestMessage, sizeof(TestMessage));
			memcpy(MessageBody(rsp_msg), MessageBody(req_msg), MessageSize(req_msg));
		}
		break;
		case MSG_TestMessageLoop: {
			TestMessageLoop *req_body = (TestMessageLoop *)MessageBody(req_msg);
			rsp_msg                   = InitResponseMessage(MSG_TestMessageLoop, sizeof(TestMessageLoop));
			TestMessageLoop *rsp_body = (TestMessageLoop *)MessageBody(rsp_msg);
			rsp_body->a               = req_body->a + 1;
		}
		break;
		case MSG_REFRESH_TABLE_SIZE: {
			rsp_msg = refresh_table_size((ReqMsgRefreshTableSize *)MessageBody(req_msg));
		}
		break;
		case MSG_REFRESH_QUOTA_INFO: {
			rsp_msg = refresh_quota_info((ReqMsgRefreshQuotaInfo *)MessageBody(req_msg));
		}
		break;
		case MSG_SET_QUOTA_CONFIG: {
		}
		break;
		default:
			break;
	}
		// GPDB6 opend a MemoryAccount for us without asking us.
		// and GPDB6 did not release the MemoryAccount after SPI finish.
		// Reset the MemoryAccount although we never create it.
#if GP_VERSION_NUM < 70000
	MemoryAccounting_Reset();
#endif /* GP_VERSION_NUM */
	return rsp_msg;
}

/*
 * how to refresh table size map:
 * - send table oid list
 * 	- remove absent table
 * - send delta table size with namespace/tablespace/role
 * 	- update table size
 * 	- get old_namespace stored in table_size_map
 * 	- get new_namespace sent by bgworker
 * 	- if old_namespace != new_namespace: update quota size
 */
static DiskquotaMessage *
refresh_table_size(ReqMsgRefreshTableSize *req_msg_body)
{
	/* request */
	Oid               dbid;
	int               segcount;
	int               oid_list_len;
	int               table_size_map_num;
	TableSizeEntry   *entry;
	TableSizeEntryKey key;
	TableSizeEntry   *new_entry;
	bool              found;
	HashMap          *table_size_map;
	HashMap          *quota_info_map;
	HASH_SEQ_STATUS   iter;
	/* response */
	DiskquotaMessage       *rsp_msg;
	RspMsgRefreshTableSize *rsp_msg_body;
	Size                    rsp_msg_size = 0;
	QuotaInfoEntry         *qentry;
	List                   *quota_info_entry_list = NIL;

	dbid               = req_msg_body->dbid;
	segcount           = req_msg_body->segcount;
	oid_list_len       = req_msg_body->oid_list_len;
	table_size_map_num = req_msg_body->table_size_entry_list_len;

	table_size_map = search_toc_map(toc_map, TABLE_SIZE_MAP, dbid);
	quota_info_map = search_toc_map(toc_map, QUOTA_INFO_MAP, dbid);

	/* set the TABLE_EXIST flag for existing relations. */
	for (int i = 0; i < oid_list_len; i++)
	{
		CopyValueFromMessageContentList(req_msg_body, req_msg_body->oid_list_offset, &key.reloid, i, sizeof(Oid));
		for (int segid = -1; segid < segcount; segid += SEGMENT_SIZE_ARRAY_LENGTH)
		{
			key.id = TableSizeEntryId(segid);
			entry  = hash_search(table_size_map->map, &key, HASH_FIND, &found);
			if (!found) break;
			set_table_size_entry_flag(entry, TABLE_EXIST);
		}
	}

	/* update table size */
	for (int i = 0; i < table_size_map_num; i++)
	{
		new_entry = (TableSizeEntry *)GetPointFromMessageContentList(
		        req_msg_body, req_msg_body->table_size_entry_list_offset, i, sizeof(TableSizeEntry));
		entry = hash_search(table_size_map->map, &new_entry->key, HASH_ENTER, &found);
		if (!found)
		{
			memset(entry->totalsize, 0, sizeof(entry->totalsize));
			entry->owneroid      = InvalidOid;
			entry->namespaceoid  = InvalidOid;
			entry->tablespaceoid = InvalidOid;
			entry->flag          = 0;
		}

		int seg_st = TableSizeEntrySegidStart(entry);
		int seg_ed = TableSizeEntrySegidEnd(entry);
		for (int segid = seg_st; segid < seg_ed; segid++)
		{
			Size new_size     = TableSizeEntryGetSize(new_entry, segid);
			Size updated_size = new_size - TableSizeEntryGetSize(entry, segid);
			TableSizeEntrySetSize(entry, segid, new_size);

			/* update the disk usage, there may be entries in the map whose keys are InvlidOid as the entry does
			 * not exist in the table_size_map */
			update_size_for_quota(quota_info_map->map, updated_size, NAMESPACE_QUOTA, (Oid[]){entry->namespaceoid},
			                      segid);
			update_size_for_quota(quota_info_map->map, updated_size, ROLE_QUOTA, (Oid[]){entry->owneroid}, segid);
			update_size_for_quota(quota_info_map->map, updated_size, ROLE_TABLESPACE_QUOTA,
			                      (Oid[]){entry->owneroid, entry->tablespaceoid}, segid);
			update_size_for_quota(quota_info_map->map, updated_size, NAMESPACE_TABLESPACE_QUOTA,
			                      (Oid[]){entry->namespaceoid, entry->tablespaceoid}, segid);

			/* if schema change, transfer the file size */
			if (entry->namespaceoid != new_entry->namespaceoid)
			{
				transfer_table_for_quota(quota_info_map->map, TableSizeEntryGetSize(entry, segid), NAMESPACE_QUOTA,
				                         (Oid[]){entry->namespaceoid}, (Oid[]){new_entry->namespaceoid}, segid);
			}
			/* if owner change, transfer the file size */
			if (entry->owneroid != new_entry->owneroid)
			{
				transfer_table_for_quota(quota_info_map->map, TableSizeEntryGetSize(entry, segid), ROLE_QUOTA,
				                         (Oid[]){entry->owneroid}, (Oid[]){new_entry->owneroid}, segid);
			}

			if (entry->tablespaceoid != new_entry->tablespaceoid || entry->namespaceoid != new_entry->namespaceoid)
			{
				transfer_table_for_quota(quota_info_map->map, TableSizeEntryGetSize(entry, segid),
				                         NAMESPACE_TABLESPACE_QUOTA, (Oid[]){entry->namespaceoid, entry->tablespaceoid},
				                         (Oid[]){new_entry->namespaceoid, new_entry->tablespaceoid}, segid);
			}
			if (entry->tablespaceoid != new_entry->tablespaceoid || entry->owneroid != new_entry->owneroid)
			{
				transfer_table_for_quota(quota_info_map->map, TableSizeEntryGetSize(entry, segid),
				                         ROLE_TABLESPACE_QUOTA, (Oid[]){entry->owneroid, entry->tablespaceoid},
				                         (Oid[]){new_entry->owneroid, new_entry->tablespaceoid}, segid);
			}
		}

		entry->namespaceoid  = new_entry->namespaceoid;
		entry->owneroid      = new_entry->owneroid;
		entry->tablespaceoid = new_entry->tablespaceoid;
	}

	/* remove absent table from table size map */
	hash_seq_init(&iter, table_size_map->map);
	while ((entry = hash_seq_search(&iter)) != NULL)
	{
		if (!get_table_size_entry_flag(entry, TABLE_EXIST))
		{
			int seg_st = TableSizeEntrySegidStart(entry);
			int seg_ed = TableSizeEntrySegidEnd(entry);
			for (int i = seg_st; i < seg_ed; i++)
			{
				update_size_for_quota(quota_info_map->map, -TableSizeEntryGetSize(entry, i), NAMESPACE_QUOTA,
				                      (Oid[]){entry->namespaceoid}, i);
				update_size_for_quota(quota_info_map->map, -TableSizeEntryGetSize(entry, i), ROLE_QUOTA,
				                      (Oid[]){entry->owneroid}, i);
				update_size_for_quota(quota_info_map->map, -TableSizeEntryGetSize(entry, i), ROLE_TABLESPACE_QUOTA,
				                      (Oid[]){entry->owneroid, entry->tablespaceoid}, i);
				update_size_for_quota(quota_info_map->map, -TableSizeEntryGetSize(entry, i), NAMESPACE_TABLESPACE_QUOTA,
				                      (Oid[]){entry->namespaceoid, entry->tablespaceoid}, i);
			}
			hash_search(table_size_map->map, &entry->key, HASH_REMOVE, NULL);
		}
		else
		{
			reset_table_size_entry_flag(entry, TABLE_EXIST);
		}
	}

	/* update the quota limit for quota_info_map */
	load_quotas(dbid, segcount, quota_info_map->map);

	/* return the current quota information to bgworker */
	hash_seq_init(&iter, quota_info_map->map);
	while ((qentry = hash_seq_search(&iter)) != NULL)
	{
		if (qentry->key.segid == -1) quota_info_entry_list = lappend(quota_info_entry_list, &qentry->key);
	}

	/*
	 * message content:
	 * - RspMsgRefreshTableSize
	 * - QuotaInfoEntryKey list
	 */
	rsp_msg_size = add_size(rsp_msg_size, sizeof(RspMsgRefreshTableSize));
	rsp_msg_size = add_size(rsp_msg_size, sizeof(QuotaInfoEntryKey) * list_length(quota_info_entry_list));
	/* initialize response message */
	rsp_msg      = InitResponseMessage(MSG_REFRESH_TABLE_SIZE, rsp_msg_size);
	rsp_msg_body = (RspMsgRefreshTableSize *)MessageBody(rsp_msg);
	/* fill message content in meesage body */
	rsp_msg_body->quota_info_entry_list_len    = list_length(quota_info_entry_list);
	rsp_msg_body->quota_info_entry_list_offset = sizeof(RspMsgRefreshTableSize);
	fill_message_content_by_list(MessageContentListAddr(rsp_msg_body, rsp_msg_body->quota_info_entry_list_offset),
	                             quota_info_entry_list, sizeof(QuotaInfoEntryKey));

	list_free(quota_info_entry_list);

	return rsp_msg;
}

static DiskquotaMessage *
refresh_quota_info(ReqMsgRefreshQuotaInfo *req_msg_body)
{
	Oid                dbid;
	int                expired_quota_info_entry_list_len;
	QuotaInfoEntryKey *key;
	HashMap           *quota_info_map;
	HASH_SEQ_STATUS    iter;
	QuotaInfoEntry    *entry;

	dbid                              = req_msg_body->dbid;
	expired_quota_info_entry_list_len = req_msg_body->expired_quota_info_entry_list_len;

	quota_info_map = search_toc_map(toc_map, QUOTA_INFO_MAP, dbid);

	/* remove expired quota info entry */
	for (int i = 0; i < expired_quota_info_entry_list_len; i++)
	{
		key = (QuotaInfoEntryKey *)GetPointFromMessageContentList(
		        req_msg_body, req_msg_body->expired_quota_info_entry_list_offset, i, sizeof(QuotaInfoEntryKey));
		hash_search(quota_info_map->map, key, HASH_REMOVE, NULL);
	}

	// TODO: update reject map by quota info map
	// TODO: send reject map to bgworker
}

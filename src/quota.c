/* -------------------------------------------------------------------------
 *
 * quota.c
 *
 * Copyright (c) 2020-Present VMware, Inc. or its affiliates
 *
 * IDENTIFICATION
 *		diskquota/quota.c
 *
 * -------------------------------------------------------------------------
 */
#include <math.h>

#include "postgres.h"

#include "access/xact.h"
#include "utils/snapmgr.h"
#include "utils/hsearch.h"
#include "utils/syscache.h"
#include "executor/spi.h"
#include "utils/elog.h"

#include "diskquota.h"
#include "quota_config.h"
#include "diskquota_guc.h"
#include "quota.h"

/*
 * quota_key_num array contains the number of key for each type of quota.
 * |----------------------------|---------------|
 * | Quota Type			 		| Number of Key |
 * |----------------------------|---------------|
 * | NAMESPACE_QUOTA			| 		1		|
 * | ROLE_QUOTA		 			| 		1		|
 * | NAMESPACE_TABLESPACE_QUOTA | 		2		|
 * | ROLE_TABLESPACE_QUOTA 		| 		2		|
 * | TABLESPACE_QUOTA 			| 		1		|
 * |----------------------------|---------------|
 */
static uint16 quota_key_num[NUM_QUOTA_TYPES]                       = {1, 1, 2, 2, 1};
static Oid    quota_key_caches[NUM_QUOTA_TYPES][MAX_QUOTA_KEY_NUM] = {
        {NAMESPACEOID}, {AUTHOID}, {NAMESPACEOID, TABLESPACEOID}, {AUTHOID, TABLESPACEOID}, {TABLESPACEOID}};
HTAB *quota_info_map;

static void do_load_quotas(void);

/* quota config function */
extern HTAB *pull_quota_config(bool *found);

Size
quota_info_map_shmem_size(void)
{
	return hash_estimate_size(MAX_QUOTA_MAP_ENTRIES, sizeof(QuotaInfoEntry)) * diskquota_max_monitored_databases;
}

void
init_quota_info_map(uint32 id)
{
	HASHCTL        hash_ctl;
	StringInfoData str;

	initStringInfo(&str);
	appendStringInfo(&str, "QuotaInfoMap_%u", id);
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.entrysize = sizeof(QuotaInfoEntry);
	hash_ctl.keysize   = sizeof(QuotaInfoEntryKey);
	quota_info_map     = DiskquotaShmemInitHash(str.data, INIT_QUOTA_MAP_ENTRIES, MAX_QUOTA_MAP_ENTRIES, &hash_ctl,
	                                            HASH_ELEM, DISKQUOTA_TAG_HASH);
	pfree(str.data);
}

void
vacuum_quota_info_map(uint32 id)
{
	HASHCTL         hash_ctl;
	HASH_SEQ_STATUS iter;
	StringInfoData  str;
	QuotaInfoEntry *qentry;

	initStringInfo(&str);
	appendStringInfo(&str, "QuotaInfoMap_%u", id);
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.entrysize = sizeof(QuotaInfoEntry);
	hash_ctl.keysize   = sizeof(QuotaInfoEntryKey);
	quota_info_map     = DiskquotaShmemInitHash(str.data, INIT_QUOTA_MAP_ENTRIES, MAX_QUOTA_MAP_ENTRIES, &hash_ctl,
	                                            HASH_ELEM, DISKQUOTA_TAG_HASH);
	hash_seq_init(&iter, quota_info_map);
	while ((qentry = hash_seq_search(&iter)) != NULL)
	{
		hash_search(quota_info_map, &qentry->key, HASH_REMOVE, NULL);
	}
	pfree(str.data);
}

/* add a new entry quota or update the old entry quota */
void
update_size_for_quota(int64 size, QuotaType type, Oid *keys, int16 segid)
{
	bool              found;
	QuotaInfoEntry   *entry;
	QuotaInfoEntryKey key = {0};

	memcpy(key.keys, keys, quota_key_num[type] * sizeof(Oid));
	key.type  = type;
	key.segid = segid;
	entry     = hash_search(quota_info_map, &key, HASH_ENTER, &found);
	if (!found)
	{
		entry->size  = 0;
		entry->limit = -1;
	}
	entry->size += size;
}

/* add a new entry quota or update the old entry limit */
void
update_limit_for_quota(int64 limit, float segratio, QuotaType type, Oid *keys)
{
	bool found;
	for (int i = -1; i < SEGCOUNT; i++)
	{
		QuotaInfoEntry   *entry;
		QuotaInfoEntryKey key = {0};

		memcpy(key.keys, keys, quota_key_num[type] * sizeof(Oid));
		key.type  = type;
		key.segid = i;
		entry     = hash_search(quota_info_map, &key, HASH_ENTER, &found);
		if (!found)
		{
			entry->size = 0;
		}
		if (key.segid == -1)
			entry->limit = limit;
		else
			entry->limit = round((limit / SEGCOUNT) * segratio);
	}
}

/* transfer one table's size from one quota to another quota */
void
transfer_table_for_quota(int64 totalsize, QuotaType type, Oid *old_keys, Oid *new_keys, int16 segid)
{
	update_size_for_quota(-totalsize, type, old_keys, segid);
	update_size_for_quota(totalsize, type, new_keys, segid);
}

void
clean_all_quota_limit(void)
{
	HASH_SEQ_STATUS iter;
	QuotaInfoEntry *entry;
	hash_seq_init(&iter, quota_info_map);
	while ((entry = hash_seq_search(&iter)) != NULL)
	{
		entry->limit = -1;
	}
}

bool
remove_expired_quota(QuotaInfoEntry *entry)
{
	HeapTuple tuple;
	QuotaType type    = entry->key.type;
	bool      removed = false;
	for (int i = 0; i < quota_key_num[type]; ++i)
	{
		tuple = SearchSysCache1(quota_key_caches[type][i], ObjectIdGetDatum(entry->key.keys[i]));
		if (!HeapTupleIsValid(tuple))
		{
			hash_search(quota_info_map, &entry->key, HASH_REMOVE, NULL);
			removed = true;
			break;
		}
		ReleaseSysCache(tuple);
	}
	return removed;
}

/*
 * Load quotas from diskquota configuration table(quota_config).
 */
static void
do_load_quotas(void)
{
	QuotaConfigKey  key = {0};
	QuotaConfig    *entry;
	QuotaConfig    *tablespace_quota_config_entry;
	HTAB           *quota_config_map;
	HASH_SEQ_STATUS iter;

	/*
	 * TODO: we should skip to reload quota config when there is no change in
	 * quota.config. A flag in shared memory could be used to detect the quota
	 * config change.
	 */
	clean_all_quota_limit();

	/* If a row exists, an update is needed. */
	quota_config_map = pull_quota_config(NULL);

	hash_seq_init(&iter, quota_config_map);
	while ((entry = (QuotaConfig *)hash_seq_search(&iter)) != NULL)
	{
		if (entry->quota_type == TABLESPACE_QUOTA) continue;

		Oid    db_oid         = entry->keys[0];
		Oid    target_oid     = entry->keys[1];
		Oid    tablespace_oid = entry->keys[2];
		int    quota_type     = entry->quota_type;
		int64  quota_limit_mb = entry->quota_limit_mb;
		float4 segratio       = INVALID_SEGRATIO;

		if (quota_type == NAMESPACE_TABLESPACE_QUOTA || quota_type == ROLE_TABLESPACE_QUOTA)
		{
			key.quota_type                = TABLESPACE_QUOTA;
			key.keys[0]                   = db_oid;
			key.keys[1]                   = tablespace_oid;
			tablespace_quota_config_entry = hash_search(quota_config_map, &key, HASH_FIND, NULL);
			if (tablespace_quota_config_entry != NULL)
			{
				segratio = tablespace_quota_config_entry->segratio;
			}
		}

		if (!OidIsValid(tablespace_oid))
			update_limit_for_quota(quota_limit_mb * (1 << 20), segratio, quota_type, (Oid[]){target_oid});
		else
			update_limit_for_quota(quota_limit_mb * (1 << 20), segratio, quota_type,
			                       (Oid[]){target_oid, tablespace_oid});
	}
	hash_destroy(quota_config_map);
}

/*
 * Interface to load quotas from diskquota configuration table(quota_config).
 */
bool
load_quotas(void)
{
	bool connected          = false;
	bool pushed_active_snap = false;
	bool ret                = true;

	StartTransactionCommand();

	/*
	 * Cache Errors during SPI functions, for example a segment may be down
	 * and current SPI execute will fail. diskquota worker process should
	 * tolerate this kind of errors and continue to check at the next loop.
	 */
	PG_TRY();
	{
		int ret_code = SPI_connect();
		if (ret_code != SPI_OK_CONNECT)
		{
			ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
			                errmsg("[diskquota] unable to connect to execute SPI query, return code: %d", ret_code)));
		}
		connected = true;
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed_active_snap = true;
		do_load_quotas();
	}
	PG_CATCH();
	{
		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();
		EmitErrorReport();
		FlushErrorState();
		ret = false;
		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();
	}
	PG_END_TRY();
	if (connected) SPI_finish();
	if (pushed_active_snap) PopActiveSnapshot();
	if (ret)
		CommitTransactionCommand();
	else
		AbortCurrentTransaction();

	return ret;
}

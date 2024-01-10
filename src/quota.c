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

static void clean_all_quota_limit(HTAB *quota_info_map);
static void do_load_quotas(Oid dbid, int segcount, HTAB *quota_info_map);
static void update_limit_for_quota(HTAB *quota_info_map, int64 limit, float segratio, QuotaType type, Oid *keys,
                                   int segcount);

/* quota config function */
extern HTAB *pull_quota_config(bool *found);

HTAB *
create_quota_info_map(const char *name)
{
	HASHCTL ctl;
	HTAB   *quota_info_map;

	memset(&ctl, 0, sizeof(ctl));
	ctl.entrysize  = sizeof(QuotaInfoEntry);
	ctl.keysize    = sizeof(QuotaInfoEntryKey);
	ctl.hcxt       = TopMemoryContext;
	quota_info_map = diskquota_hash_create(name, 1024, &ctl, HASH_ELEM | HASH_CONTEXT, DISKQUOTA_TAG_HASH);
	return quota_info_map;
}

// TODO: vacuum quota_info_map after dropping extension
void
vacuum_quota_info_map(HTAB *quota_info_map)
{
	hash_destroy(quota_info_map);
}

/* add a new entry quota or update the old entry quota */
void
update_size_for_quota(HTAB *quota_info_map, int64 size, QuotaType type, Oid *keys, int16 segid)
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
static void
update_limit_for_quota(HTAB *quota_info_map, int64 limit, float segratio, QuotaType type, Oid *keys, int segcount)
{
	QuotaInfoEntry   *entry;
	QuotaInfoEntryKey key = {0};
	bool              found;

	memcpy(key.keys, keys, quota_key_num[type] * sizeof(Oid));
	key.type = type;
	if (is_quota_expired(&key)) return;

	for (int i = -1; i < SEGCOUNT; i++)
	{
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
transfer_table_for_quota(HTAB *quota_info_map, int64 totalsize, QuotaType type, Oid *old_keys, Oid *new_keys,
                         int16 segid)
{
	update_size_for_quota(quota_info_map, -totalsize, type, old_keys, segid);
	update_size_for_quota(quota_info_map, totalsize, type, new_keys, segid);
}

static void
clean_all_quota_limit(HTAB *quota_info_map)
{
	HASH_SEQ_STATUS iter;
	QuotaInfoEntry *entry;
	hash_seq_init(&iter, quota_info_map);
	while ((entry = hash_seq_search(&iter)) != NULL)
	{
		entry->limit = -1;
	}
}

/*
 * Load quotas from diskquota configuration table(quota_config).
 */
static void
do_load_quotas(Oid dbid, int segcount, HTAB *quota_info_map)
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
	clean_all_quota_limit(quota_info_map);

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

		// TODO: pull quota_config_map for certain database
		if (db_oid != dbid) continue;

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
			update_limit_for_quota(quota_info_map, quota_limit_mb * (1 << 20), segratio, quota_type,
			                       (Oid[]){target_oid}, segcount);
		else
			update_limit_for_quota(quota_info_map, quota_limit_mb * (1 << 20), segratio, quota_type,
			                       (Oid[]){target_oid, tablespace_oid}, segcount);
	}
	hash_destroy(quota_config_map);
}

/*
 * Interface to load quotas from diskquota configuration table(quota_config).
 */
bool
load_quotas(Oid dbid, int segcount, HTAB *quota_info_map)
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
		do_load_quotas(dbid, segcount, quota_info_map);
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

bool
is_quota_expired(QuotaInfoEntryKey *key)
{
	HeapTuple tuple;
	QuotaType type    = key->type;
	bool      removed = false;
	for (int i = 0; i < quota_key_num[type]; ++i)
	{
		tuple = SearchSysCache1(quota_key_caches[type][i], ObjectIdGetDatum(key->keys[i]));
		if (!HeapTupleIsValid(tuple))
		{
			removed = true;
			break;
		}
		ReleaseSysCache(tuple);
	}
	return removed;
}

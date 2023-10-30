/* -------------------------------------------------------------------------
 *
 * quota.h
 *
 * Copyright (c) 2020-Present VMware, Inc. or its affiliates
 *
 * IDENTIFICATION
 *		diskquota/quota.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef QUOTA_H
#define QUOTA_H

#include "quota_config.h"

/* init number of QuotaInfoEntry in quota_info_map */
#define INIT_QUOTA_MAP_ENTRIES 128
#define AVG_QUOTA_MAP_ENTRIES (diskquota_max_quota_probes / diskquota_max_monitored_databases)
/* max number of QuotaInfoEntry in quota_info_map */
#define MAX_QUOTA_MAP_ENTRIES (AVG_QUOTA_MAP_ENTRIES < 1024 ? 1024 : AVG_QUOTA_MAP_ENTRIES)

/*
 * table disk size and corresponding schema, owner and tablespace
 */
typedef struct QuotaInfoEntryKey
{
	QuotaType type;
	Oid       keys[MAX_QUOTA_KEY_NUM];
	int16     segid;
} QuotaInfoEntryKey;

typedef struct QuotaInfoEntry
{
	QuotaInfoEntryKey key;
	int64             size;
	int64             limit;
} QuotaInfoEntry;

extern HTAB *quota_info_map;

extern Size quota_info_map_shmem_size(void);
extern void init_quota_info_map(uint32 id);
extern void vacuum_quota_info_map(uint32 id);

extern void update_size_for_quota(int64 size, QuotaType type, Oid *keys, int16 segid);
extern void update_limit_for_quota(int64 limit, float segratio, QuotaType type, Oid *keys);
extern void transfer_table_for_quota(int64 totalsize, QuotaType type, Oid *old_keys, Oid *new_keys, int16 segid);
extern void clean_all_quota_limit(void);
extern bool remove_expired_quota(QuotaInfoEntry *entry);

extern bool load_quotas(void);

#endif
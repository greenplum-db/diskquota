/* -------------------------------------------------------------------------
 *
 * diskquota_stat.h
 *
 * Copyright (c) 2023-Present VMware, Inc. or its affiliates
 *
 * IDENTIFICATION
 *		diskquota/diskquota_stat.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef DISKQUOTA_STAT_H
#define DISKQUOTA_STAT_H

#include "c.h"
#include "port/atomics.h"

typedef struct DiskquotaStatus
{
	/* TableSizeEntry */
	pg_atomic_uint32 total_table_size_entries; /* how many TableSizeEntry are maintained in all the table_size_map in
	                                               shared memory*/
	uint32 *allocated_table_size_entries;
	uint32  table_size_entry_nelem_alloc;
	/* QuotaInfoEntry */
	pg_atomic_uint32 total_quota_info_entries; /* how many QuotaInfoEntry are maintained in all the quota_info[type].map
	                                               in shared memory*/
	uint32 *allocated_quota_info_entries;
	uint32  quota_info_entry_nelem_alloc;
} DiskquotaStatus;

extern void init_diskquota_status(uint32 id);
extern Size diskquota_status_shmem_size(void);

/* table_size_map */
extern bool        alloc_table_size_entry(HTAB *map);
extern bool        table_size_entry_exceed_limit(HTAB *map);
extern const char *diskquota_status_table_size_entry(void);

/* quota_info_map */
extern bool        alloc_quota_info_entry(HTAB *map);
extern bool        quota_info_entry_exceed_limit(HTAB *map);
extern const char *diskquota_status_quota_info_entry(void);

#endif

#include "postgres.h"
#include "utils/hsearch.h"

#include "diskquota.h"
#include "diskquota_stat.h"

#include <math.h>

/* GUC variable */
extern int diskquota_max_monitored_databases;
extern int diskquota_max_table_segments;
extern int diskquota_max_quota_probes;

extern uint32 diskquota_current_worker_id;

DiskquotaStatus *diskquota_stat;

/*
 * Copy from gp6.
 * Given the user-specified entry size, choose nelem_alloc, ie, how many
 * elements to add to the hash table when we need more.
 */
static int
choose_nelem_alloc(Size entrysize)
{
	int  nelem_alloc;
	Size elementSize;
	Size allocSize;

	/* Each element has a HASHELEMENT header plus user data. */
	/* NB: this had better match element_alloc() */
	elementSize = MAXALIGN(sizeof(HASHELEMENT)) + MAXALIGN(entrysize);

	/*
	 * The idea here is to choose nelem_alloc at least 32, but round up so
	 * that the allocation request will be a power of 2 or just less. This
	 * makes little difference for hash tables in shared memory, but for hash
	 * tables managed by palloc, the allocation request will be rounded up to
	 * a power of 2 anyway.  If we fail to take this into account, we'll waste
	 * as much as half the allocated space.
	 */
	allocSize = 32 * 4; /* assume elementSize at least 8 */
	do
	{
		allocSize <<= 1;
		nelem_alloc = allocSize / elementSize;
	} while (nelem_alloc < 32);

	return nelem_alloc;
}

Size
diskquota_status_shmem_size(void)
{
	Size sz = 0;

	sz = add_size(sz, sizeof(DiskquotaStatus));
	sz = add_size(sz, diskquota_max_monitored_databases * sizeof(uint32));
	sz = add_size(sz, diskquota_max_monitored_databases * sizeof(uint32));

	return sz;
}

void
init_diskquota_status(uint32 id)
{
	bool found;
	int  i;

	diskquota_stat = ShmemInitStruct("diskquota status struct", sizeof(DiskquotaStatus), &found);
	/* initalize diskquota_stat */
	if (!found)
	{
		pg_atomic_init_u32(&diskquota_stat->total_table_size_entries, 0);
		pg_atomic_init_u32(&diskquota_stat->total_quota_info_entries, 0);
		diskquota_stat->allocated_table_size_entries = ShmemInitStruct(
		        "diskquota_allocated_table_size_entries", diskquota_max_monitored_databases * sizeof(uint32), &found);
		diskquota_stat->allocated_quota_info_entries = ShmemInitStruct(
		        "diskquota_allocated_quota_info_entries", diskquota_max_monitored_databases * sizeof(uint32), &found);
		for (i = 0; i < diskquota_max_monitored_databases; i++)
		{
			diskquota_stat->allocated_table_size_entries[i] = 0;
			diskquota_stat->allocated_quota_info_entries[i] = 0;
		}
		diskquota_stat->table_size_entry_nelem_alloc = choose_nelem_alloc(sizeof(TableSizeEntry));
		diskquota_stat->quota_info_entry_nelem_alloc = choose_nelem_alloc(sizeof(QuotaInfoEntry));
	}
	if (id == -1) return;
	/* The hash table in the current bgworker is uninitialized. */
	if (diskquota_stat->allocated_table_size_entries[id] == 0)
	{
		diskquota_stat->allocated_table_size_entries[id] = INIT_NUM_TABLE_SIZE_ENTRIES;
		diskquota_stat->allocated_quota_info_entries[id] = INIT_QUOTA_MAP_ENTRIES;
		pg_atomic_add_fetch_u32(&diskquota_stat->total_table_size_entries, INIT_NUM_TABLE_SIZE_ENTRIES);
		pg_atomic_add_fetch_u32(&diskquota_stat->total_quota_info_entries, INIT_QUOTA_MAP_ENTRIES);
	}
}

/*
 * TableSizeEntry status
 */

/*
 * Increase the count of memory allocated for TableSizeEntry.
 * IF used_count = allocated_count, we need to allocate more memory and increase
 * allocated_count. And allocated_count cannot exceed MAX_NUM_TABLE_SIZE_ENTRIES.
 */
bool
alloc_table_size_entry(HTAB *map)
{
	uint32 used      = hash_get_num_entries(map);
	uint32 allocated = diskquota_stat->allocated_table_size_entries[diskquota_current_worker_id];
	if (used < allocated) return true;
	/* If total_table_size_entries exceeds the limit of TableSizeEntry, skip insert. */
	uint32 total = pg_atomic_read_u32(&diskquota_stat->total_table_size_entries);
	if (total >= MAX_NUM_TABLE_SIZE_ENTRIES) return false;
	/* If other workers increase the total value and make it exceed the limit, skip insert. */
	total = pg_atomic_add_fetch_u32(&diskquota_stat->total_table_size_entries,
	                                diskquota_stat->table_size_entry_nelem_alloc);
	if (total > MAX_NUM_TABLE_SIZE_ENTRIES) return false;
	/* Increase the allocated count and used count */
	diskquota_stat->allocated_table_size_entries[diskquota_current_worker_id] +=
	        diskquota_stat->table_size_entry_nelem_alloc;
	return true;
}

bool
table_size_entry_exceed_limit(HTAB *map)
{
	uint32 used      = hash_get_num_entries(map);
	uint32 allocated = diskquota_stat->allocated_table_size_entries[diskquota_current_worker_id];
	if (used < allocated) return false;
	uint32 total = pg_atomic_read_u32(&diskquota_stat->total_table_size_entries);
	return total >= MAX_NUM_TABLE_SIZE_ENTRIES;
}

const char *
diskquota_status_table_size_entry(void)
{
	static char ret[10];
	init_diskquota_status(-1);
	int    used     = pg_atomic_read_u32(&diskquota_stat->total_table_size_entries);
	float4 capacity = fabs(1.0 * used / MAX_NUM_TABLE_SIZE_ENTRIES);
	sprintf(ret, "%.2f%%", capacity);
	return ret;
}

/*
 * QuotaInfoEntry
 */

/*
 * Increase the count of memory allocated for QuotaInfoEntry.
 * IF used_count = allocated_count, we need to allocate more memory and increase
 * allocated_count. And allocated_count cannot exceed diskquota_max_quota_probes.
 */
bool
alloc_quota_info_entry(HTAB *map)
{
	uint32 used      = hash_get_num_entries(map);
	uint32 allocated = diskquota_stat->allocated_quota_info_entries[diskquota_current_worker_id];
	if (used < allocated) return true;
	/* If total_quota_info_entries exceeds the limit of QuotaInfoEntry, skip insert. */
	uint32 total = pg_atomic_read_u32(&diskquota_stat->total_quota_info_entries);
	if (total >= diskquota_max_quota_probes) return false;
	/* If other workers increase the total value and make it exceed the limit, skip insert. */
	total = pg_atomic_add_fetch_u32(&diskquota_stat->total_quota_info_entries,
	                                diskquota_stat->quota_info_entry_nelem_alloc);
	if (total > diskquota_max_quota_probes) return false;
	/* Increase the allocated count and used count */
	diskquota_stat->allocated_quota_info_entries[diskquota_current_worker_id] +=
	        diskquota_stat->quota_info_entry_nelem_alloc;
	return true;
}

bool
quota_info_entry_exceed_limit(HTAB *map)
{
	uint32 used      = hash_get_num_entries(map);
	uint32 allocated = diskquota_stat->allocated_quota_info_entries[diskquota_current_worker_id];
	if (used < allocated) return false;
	uint32 total = pg_atomic_read_u32(&diskquota_stat->total_quota_info_entries);
	return total >= diskquota_max_quota_probes;
}

const char *
diskquota_status_quota_info_entry(void)
{
	static char ret[10];
	init_diskquota_status(-1);
	int    used     = pg_atomic_read_u32(&diskquota_stat->total_quota_info_entries);
	float4 capacity = fabs(1.0 * used / diskquota_max_quota_probes);
	sprintf(ret, "%.2f%%", capacity);
	return ret;
}

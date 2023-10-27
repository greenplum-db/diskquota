/* -------------------------------------------------------------------------
 *
 * table_size.h
 *
 * Copyright (c) 2020-Present VMware, Inc. or its affiliates
 *
 * IDENTIFICATION
 *		diskquota/table_size.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef TABLE_SIZE_H
#define TABLE_SIZE_H

#include "postgres.h"
#include "diskquota_guc.h"
#include "diskquota.h"

/* init number of TableSizeEntry in table_size_map */
#define INIT_NUM_TABLE_SIZE_ENTRIES 128
/* max number of TableSizeEntry in table_size_map */
#define MAX_NUM_TABLE_SIZE_ENTRIES (diskquota_max_table_segments / SEGMENT_SIZE_ARRAY_LENGTH)
/* length of segment size array in TableSizeEntry */
#define SEGMENT_SIZE_ARRAY_LENGTH 100
/* Number of entries for diskquota.table_size update SQL */
#define SQL_MAX_VALUES_NUMBER 1000000

/* TableSizeEntry macro function */
/* Use the top bit of totalsize as a flush flag. If this bit is set, the size should be flushed into
 * diskquota.table_size_table. */
#define TableSizeEntryFlushFlag (1ul << 63)
#define TableSizeEntrySizeMask (TableSizeEntryFlushFlag - 1)
#define TableSizeEntryId(segid) ((segid + 1) / SEGMENT_SIZE_ARRAY_LENGTH)
#define TableSizeEntryIndex(segid) ((segid + 1) % SEGMENT_SIZE_ARRAY_LENGTH)
#define TableSizeEntryGetFlushFlag(entry, segid) \
	(entry->totalsize[TableSizeEntryIndex(segid)] & TableSizeEntryFlushFlag)
#define TableSizeEntrySetFlushFlag(entry, segid) entry->totalsize[TableSizeEntryIndex(segid)] |= TableSizeEntryFlushFlag
#define TableSizeEntryResetFlushFlag(entry, segid) \
	entry->totalsize[TableSizeEntryIndex(segid)] &= TableSizeEntrySizeMask
#define TableSizeEntryGetSize(entry, segid) (entry->totalsize[TableSizeEntryIndex(segid)] & TableSizeEntrySizeMask)
#define TableSizeEntrySetSize(entry, segid, size) entry->totalsize[TableSizeEntryIndex(segid)] = size
#define TableSizeEntrySegidStart(entry) (entry->key.id * SEGMENT_SIZE_ARRAY_LENGTH - 1)
#define TableSizeEntrySegidEnd(entry)                                 \
	(((entry->key.id + 1) * SEGMENT_SIZE_ARRAY_LENGTH - 1) < SEGCOUNT \
	         ? ((entry->key.id + 1) * SEGMENT_SIZE_ARRAY_LENGTH - 1)  \
	         : SEGCOUNT)

/*
 * local cache of table disk size and corresponding schema and owner.
 *
 * When id is 0, this TableSizeEntry stores the table size in the (-1 ~
 * SEGMENT_SIZE_ARRAY_LENGTH - 2)th segment, and so on.
 * |---------|--------------------------------------------------------------------------|
 * |   id    |                                segment index                             |
 * |---------|--------------------------------------------------------------------------|
 * |    0    |  [-1,                                SEGMENT_SIZE_ARRAY_LENGTH - 1)      |
 * |    1    |  [SEGMENT_SIZE_ARRAY_LENGTH - 1,     2 * SEGMENT_SIZE_ARRAY_LENGTH - 1)  |
 * |    2    |  [2 * SEGMENT_SIZE_ARRAY_LENGTH - 1, 3 * SEGMENT_SIZE_ARRAY_LENGTH - 1)  |
 * --------------------------------------------------------------------------------------
 *
 * flag's each bit is used to show the table's status, which is described in TableSizeEntryFlag.
 *
 * totalsize contains tables' size on segments. When id is 0, totalsize[0] is the sum of all segments' table size.
 * table size including fsm, visibility map etc.
 */
typedef struct TableSizeEntryKey
{
	Oid reloid;
	int id;
} TableSizeEntryKey;

typedef struct TableSizeEntry
{
	TableSizeEntryKey key;
	Oid               tablespaceoid;
	Oid               namespaceoid;
	Oid               owneroid;
	uint32            flag;
	int64             totalsize[SEGMENT_SIZE_ARRAY_LENGTH];
} TableSizeEntry;

typedef enum
{
	TABLE_EXIST = (1 << 0), /* whether table is already dropped */
} TableSizeEntryFlag;

extern HTAB *table_size_map;

extern void init_table_size_map(uint32 id);
extern void vacuum_table_size_map(uint32 id);
extern Size diskquota_table_size_shmem_size(void);

extern bool get_table_size_entry_flag(TableSizeEntry *entry, TableSizeEntryFlag flag);
extern void reset_table_size_entry_flag(TableSizeEntry *entry, TableSizeEntryFlag flag);
extern void set_table_size_entry_flag(TableSizeEntry *entry, TableSizeEntryFlag flag);

extern void flush_to_table_size(void);

#endif

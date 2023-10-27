/* -------------------------------------------------------------------------
 *
 * table_size.c
 *
 * Copyright (c) 2020-Present VMware, Inc. or its affiliates
 *
 * IDENTIFICATION
 *		diskquota/table_size.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "table_size.h"

#include "executor/spi.h"
#include "utils/hsearch.h"
#include "utils/guc.h"

/* using hash table to support incremental update the table size entry.*/
HTAB *table_size_map = NULL;

static void insert_into_table_size_map(char *str);
static void delete_from_table_size_map(char *str);

Size
diskquota_table_size_shmem_size(void)
{
	return hash_estimate_size(MAX_NUM_TABLE_SIZE_ENTRIES / diskquota_max_monitored_databases + 100,
	                          sizeof(TableSizeEntry));
}

void
init_table_size_map(uint32 id)
{
	HASHCTL        hash_ctl;
	StringInfoData str;

	initStringInfo(&str);
	appendStringInfo(&str, "TableSizeEntrymap_%u", id);
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(TableSizeEntryKey);
	hash_ctl.entrysize = sizeof(TableSizeEntry);
	table_size_map     = DiskquotaShmemInitHash(str.data, INIT_NUM_TABLE_SIZE_ENTRIES, MAX_NUM_TABLE_SIZE_ENTRIES,
	                                            &hash_ctl, HASH_ELEM, DISKQUOTA_TAG_HASH);
}

void
vacuum_table_size_map(uint32 id)
{
	HASHCTL         hash_ctl;
	HASH_SEQ_STATUS iter;
	StringInfoData  str;
	TableSizeEntry *tsentry;

	initStringInfo(&str);
	appendStringInfo(&str, "TableSizeEntrymap_%u", id);
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(TableSizeEntryKey);
	hash_ctl.entrysize = sizeof(TableSizeEntry);
	table_size_map     = DiskquotaShmemInitHash(str.data, INIT_NUM_TABLE_SIZE_ENTRIES, MAX_NUM_TABLE_SIZE_ENTRIES,
	                                            &hash_ctl, HASH_ELEM, DISKQUOTA_TAG_HASH);
	hash_seq_init(&iter, table_size_map);
	while ((tsentry = hash_seq_search(&iter)) != NULL)
	{
		hash_search(table_size_map, &tsentry->key, HASH_REMOVE, NULL);
	}
}

bool
get_table_size_entry_flag(TableSizeEntry *entry, TableSizeEntryFlag flag)
{
	return (entry->flag & flag) ? true : false;
}

void
reset_table_size_entry_flag(TableSizeEntry *entry, TableSizeEntryFlag flag)
{
	entry->flag &= (UINT32_MAX ^ flag);
}

void
set_table_size_entry_flag(TableSizeEntry *entry, TableSizeEntryFlag flag)
{
	entry->flag |= flag;
}

static void
delete_from_table_size_map(char *str)
{
	StringInfoData delete_statement;
	int            ret;

	initStringInfo(&delete_statement);
	appendStringInfo(&delete_statement,
	                 "WITH deleted_table AS ( VALUES %s ) "
	                 "delete from diskquota.table_size "
	                 "where (tableid, segid) in ( SELECT * FROM deleted_table );",
	                 str);
	ret = SPI_execute(delete_statement.data, false, 0);
	if (ret != SPI_OK_DELETE)
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
		                errmsg("[diskquota] delete_from_table_size_map SPI_execute failed: error code %d", ret)));
	pfree(delete_statement.data);
}

static void
insert_into_table_size_map(char *str)
{
	StringInfoData insert_statement;
	int            ret;

	initStringInfo(&insert_statement);
	appendStringInfo(&insert_statement, "insert into diskquota.table_size values %s;", str);
	ret = SPI_execute(insert_statement.data, false, 0);
	if (ret != SPI_OK_INSERT)
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
		                errmsg("[diskquota] insert_into_table_size_map SPI_execute failed: error code %d", ret)));
	pfree(insert_statement.data);
}

/*
 * Flush the table_size_map to user table diskquota.table_size
 * To improve update performance, we first delete all the need_to_flush
 * entries in table table_size. And then insert new table size entries into
 * table table_size.
 */
void
flush_to_table_size(void)
{
	HASH_SEQ_STATUS iter;
	TableSizeEntry *tsentry = NULL;
	StringInfoData  delete_statement;
	StringInfoData  insert_statement;
	int             delete_entries_num = 0;
	int             insert_entries_num = 0;

	/* TODO: Add flush_size_interval to avoid flushing size info in every loop */

	/* Disable ORCA since it does not support non-scalar subqueries. */
	bool old_optimizer = optimizer;
	optimizer          = false;

	initStringInfo(&insert_statement);
	initStringInfo(&delete_statement);

	hash_seq_init(&iter, table_size_map);
	while ((tsentry = hash_seq_search(&iter)) != NULL)
	{
		int seg_st = TableSizeEntrySegidStart(tsentry);
		int seg_ed = TableSizeEntrySegidEnd(tsentry);
		for (int i = seg_st; i < seg_ed; i++)
		{
			/* delete dropped table from both table_size_map and table table_size */
			if (!get_table_size_entry_flag(tsentry, TABLE_EXIST))
			{
				appendStringInfo(&delete_statement, "%s(%u,%d)", (delete_entries_num == 0) ? " " : ", ",
				                 tsentry->key.reloid, i);
				delete_entries_num++;
				if (delete_entries_num > SQL_MAX_VALUES_NUMBER)
				{
					delete_from_table_size_map(delete_statement.data);
					resetStringInfo(&delete_statement);
					delete_entries_num = 0;
				}
			}
			/* update the table size by delete+insert in table table_size */
			else if (TableSizeEntryGetFlushFlag(tsentry, i))
			{
				appendStringInfo(&delete_statement, "%s(%u,%d)", (delete_entries_num == 0) ? " " : ", ",
				                 tsentry->key.reloid, i);
				appendStringInfo(&insert_statement, "%s(%u,%ld,%d)", (insert_entries_num == 0) ? " " : ", ",
				                 tsentry->key.reloid, TableSizeEntryGetSize(tsentry, i), i);
				delete_entries_num++;
				insert_entries_num++;

				if (delete_entries_num > SQL_MAX_VALUES_NUMBER)
				{
					delete_from_table_size_map(delete_statement.data);
					resetStringInfo(&delete_statement);
					delete_entries_num = 0;
				}
				if (insert_entries_num > SQL_MAX_VALUES_NUMBER)
				{
					insert_into_table_size_map(insert_statement.data);
					resetStringInfo(&insert_statement);
					insert_entries_num = 0;
				}

				TableSizeEntryResetFlushFlag(tsentry, i);
			}
		}
		if (!get_table_size_entry_flag(tsentry, TABLE_EXIST))
		{
			hash_search(table_size_map, &tsentry->key, HASH_REMOVE, NULL);
		}
	}

	if (delete_entries_num) delete_from_table_size_map(delete_statement.data);
	if (insert_entries_num) insert_into_table_size_map(insert_statement.data);

	optimizer = old_optimizer;

	pfree(delete_statement.data);
	pfree(insert_statement.data);
}

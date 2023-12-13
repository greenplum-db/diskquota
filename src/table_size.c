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

#include "executor/spi.h"
#include "utils/hsearch.h"
#include "utils/guc.h"
#include "utils/syscache.h"

#include "table_size.h"
#include "quota.h"
#include "gp_activetable.h"
#include "relation_cache.h"

HTAB *
create_table_size_map(const char *name)
{
	HASHCTL ctl;
	HTAB   *table_size_map;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize    = sizeof(TableSizeEntryKey);
	ctl.entrysize  = sizeof(TableSizeEntry);
	ctl.hcxt       = CurrentMemoryContext;
	table_size_map = diskquota_hash_create(name, 1024, &ctl, HASH_ELEM | HASH_CONTEXT, DISKQUOTA_TAG_HASH);
	return table_size_map;
}

// TODO: when drop extension, vacuum table size map
// in center worker
void
vacuum_table_size_map(HTAB *table_size_map)
{
	HASH_SEQ_STATUS iter;
	TableSizeEntry *tsentry;

	hash_seq_init(&iter, table_size_map);
	while ((tsentry = hash_seq_search(&iter)) != NULL)
	{
		hash_search(table_size_map, &tsentry->key, HASH_REMOVE, NULL);
	}
}

HTAB *
get_current_database_table_size_map(HTAB *local_active_table_map)
{
	TableSizeEntryKey key;
	TableSizeEntry   *tsentry;
	ActiveTableEntry *active_table_entry;
	HASHCTL           ctl;
	HTAB             *local_table_size_map;
	bool              table_size_map_found;
	HASH_SEQ_STATUS   iter;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize   = sizeof(TableSizeEntryKey);
	ctl.entrysize = sizeof(TableSizeEntry);
	ctl.hcxt      = CurrentMemoryContext;

	local_table_size_map =
	        diskquota_hash_create("local_table_size_map", 1024, &ctl, HASH_ELEM | HASH_CONTEXT, DISKQUOTA_TAG_HASH);

	hash_seq_init(&iter, local_active_table_map);
	while ((active_table_entry = hash_seq_search(&iter)) != NULL)
	{
		Oid           relid = active_table_entry->reloid;
		int           segid = active_table_entry->segid;
		HeapTuple     classTup;
		Form_pg_class classForm     = NULL;
		Oid           relnamespace  = InvalidOid;
		Oid           relowner      = InvalidOid;
		Oid           reltablespace = InvalidOid;

		classTup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
		if (HeapTupleIsValid(classTup))
		{
			classForm     = (Form_pg_class)GETSTRUCT(classTup);
			relnamespace  = classForm->relnamespace;
			relowner      = classForm->relowner;
			reltablespace = classForm->reltablespace;

			if (!OidIsValid(reltablespace))
			{
				reltablespace = MyDatabaseTableSpace;
			}
		}
		else
		{
			LWLockAcquire(diskquota_locks.relation_cache_lock, LW_SHARED);
			DiskQuotaRelationCacheEntry *relation_entry = hash_search(relation_cache, &relid, HASH_FIND, NULL);
			if (relation_entry == NULL)
			{
				elog(WARNING, "cache lookup failed for relation %u", relid);
				LWLockRelease(diskquota_locks.relation_cache_lock);
				continue;
			}
			relnamespace  = relation_entry->namespaceoid;
			relowner      = relation_entry->owneroid;
			reltablespace = relation_entry->rnode.node.spcNode;
			LWLockRelease(diskquota_locks.relation_cache_lock);
		}

		key.reloid = relid;
		key.id     = TableSizeEntryId(segid);
		tsentry    = (TableSizeEntry *)hash_search(local_table_size_map, &key, HASH_ENTER, &table_size_map_found);

		if (!table_size_map_found)
		{
			tsentry->key.reloid = relid;
			tsentry->key.id     = key.id;
			memset(tsentry->totalsize, 0, sizeof(tsentry->totalsize));
			tsentry->owneroid      = relowner;
			tsentry->namespaceoid  = relnamespace;
			tsentry->tablespaceoid = reltablespace;
			tsentry->flag          = 0;
		}

		if (segid == -1)
		{
			active_table_entry->tablesize += calculate_table_size(relid);
		}

		TableSizeEntrySetSize(tsentry, segid, active_table_entry->tablesize);

		if (HeapTupleIsValid(classTup))
		{
			heap_freetuple(classTup);
		}
	}

	return local_table_size_map;
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

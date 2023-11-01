/* -------------------------------------------------------------------------
 *
 * rejectmap.c
 *
 * Copyright (c) 2020-Present VMware, Inc. or its affiliates
 *
 * IDENTIFICATION
 *		diskquota/rejectmap.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"
#include "utils/inval.h"
#include "utils/hsearch.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/faultinjector.h"
#include "utils/lsyscache.h"
#include "storage/relfilenode.h"
#include "miscadmin.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbdisp_query.h"
#include "cdb/cdbdispatchresult.h"
#include "funcapi.h"

#include "diskquota.h"
#include "gp_activetable.h"
#include "relation_cache.h"
#include "diskquota_util.h"
#include "rejectmap.h"

/* rejectmap for database objects which exceed their quota limit */
HTAB *disk_quota_reject_map       = NULL;
HTAB *local_disk_quota_reject_map = NULL;

static void export_exceeded_error(GlobalRejectMapEntry *entry, bool skip_name);

void
init_shm_worker_rejectmap(void)
{
	HASHCTL hash_ctl;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(RejectMapEntry);
	hash_ctl.entrysize = sizeof(GlobalRejectMapEntry);
	disk_quota_reject_map =
	        DiskquotaShmemInitHash("rejectmap whose quota limitation is reached", INIT_DISK_QUOTA_REJECT_ENTRIES,
	                               MAX_DISK_QUOTA_REJECT_ENTRIES, &hash_ctl, HASH_ELEM, DISKQUOTA_TAG_HASH);
}

void
init_local_reject_map(uint32 id)
{
	HASHCTL        hash_ctl;
	StringInfoData str;

	initStringInfo(&str);
	appendStringInfo(&str, "LocalRejectmap_%u", id);
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(RejectMapEntry);
	hash_ctl.entrysize = sizeof(LocalRejectMapEntry);
	local_disk_quota_reject_map =
	        DiskquotaShmemInitHash(str.data, MAX_LOCAL_DISK_QUOTA_REJECT_ENTRIES, MAX_LOCAL_DISK_QUOTA_REJECT_ENTRIES,
	                               &hash_ctl, HASH_ELEM, DISKQUOTA_TAG_HASH);
}

void
vacuum_local_reject_map(uint32 id)
{
	HASHCTL              hash_ctl;
	HASH_SEQ_STATUS      iter;
	StringInfoData       str;
	LocalRejectMapEntry *localrejectentry;

	initStringInfo(&str);
	appendStringInfo(&str, "LocalRejectmap_%u", id);

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(RejectMapEntry);
	hash_ctl.entrysize = sizeof(LocalRejectMapEntry);
	local_disk_quota_reject_map =
	        DiskquotaShmemInitHash(str.data, MAX_LOCAL_DISK_QUOTA_REJECT_ENTRIES, MAX_LOCAL_DISK_QUOTA_REJECT_ENTRIES,
	                               &hash_ctl, HASH_ELEM, DISKQUOTA_TAG_HASH);
	hash_seq_init(&iter, local_disk_quota_reject_map);
	while ((localrejectentry = hash_seq_search(&iter)) != NULL)
	{
		hash_search(local_disk_quota_reject_map, &localrejectentry->keyitem, HASH_REMOVE, NULL);
	}
}

Size
diskquota_rejectmap_shmem_size(void)
{
	return hash_estimate_size(MAX_DISK_QUOTA_REJECT_ENTRIES, sizeof(GlobalRejectMapEntry));
}

Size
diskquota_local_rejectmap_shmem_size(void)
{
	return hash_estimate_size(MAX_LOCAL_DISK_QUOTA_REJECT_ENTRIES, sizeof(LocalRejectMapEntry));
}

/*
 * Compare the disk quota limit and current usage of a database object.
 * Put them into local rejectmap if quota limit is exceeded.
 */
void
add_quota_to_rejectmap(QuotaType type, Oid targetOid, Oid tablespaceoid, bool segexceeded)
{
	LocalRejectMapEntry *localrejectentry;
	RejectMapEntry       keyitem = {0};

	keyitem.targetoid     = targetOid;
	keyitem.databaseoid   = MyDatabaseId;
	keyitem.tablespaceoid = tablespaceoid;
	keyitem.targettype    = (uint32)type;
	ereport(DEBUG1, (errmsg("[diskquota] Put object %u to rejectmap", targetOid)));
	localrejectentry = (LocalRejectMapEntry *)hash_search(local_disk_quota_reject_map, &keyitem, HASH_ENTER, NULL);
	localrejectentry->isexceeded  = true;
	localrejectentry->segexceeded = segexceeded;
}

/*
 * Generate the new shared rejectmap from the local_rejectmap which
 * exceed the quota limit.
 * local_rejectmap is used to reduce the lock contention.
 */
bool
flush_local_reject_map(void)
{
	bool                  changed = false;
	HASH_SEQ_STATUS       iter;
	LocalRejectMapEntry  *localrejectentry;
	GlobalRejectMapEntry *rejectentry;
	bool                  found;

	LWLockAcquire(diskquota_locks.reject_map_lock, LW_EXCLUSIVE);

	hash_seq_init(&iter, local_disk_quota_reject_map);
	while ((localrejectentry = hash_seq_search(&iter)) != NULL)
	{
		/*
		 * If localrejectentry->isexceeded is true, and it alredy exists in disk_quota_reject_map,
		 * that means the reject entry exists in both last loop and current loop, but its segexceeded
		 * feild may have changed.
		 *
		 * If localrejectentry->isexceeded is true, and it doesn't exist in disk_quota_reject_map,
		 * then it is a new added reject entry in this loop.
		 *
		 * Otherwise, it means the reject entry has gone, we need to delete it.
		 */
		if (localrejectentry->isexceeded)
		{
			rejectentry = (GlobalRejectMapEntry *)hash_search(disk_quota_reject_map, (void *)&localrejectentry->keyitem,
			                                                  HASH_ENTER_NULL, &found);
			if (rejectentry == NULL)
			{
				ereport(WARNING, (errmsg("[diskquota] Shared disk quota reject map size limit reached."
				                         "Some out-of-limit schemas or roles will be lost"
				                         "in rejectmap.")));
				continue;
			}
			/* new db objects which exceed quota limit */
			if (!found)
			{
				rejectentry->keyitem.targetoid     = localrejectentry->keyitem.targetoid;
				rejectentry->keyitem.databaseoid   = MyDatabaseId;
				rejectentry->keyitem.targettype    = localrejectentry->keyitem.targettype;
				rejectentry->keyitem.tablespaceoid = localrejectentry->keyitem.tablespaceoid;
				rejectentry->segexceeded           = localrejectentry->segexceeded;
				changed                            = true;
			}
			if (rejectentry->segexceeded != localrejectentry->segexceeded)
			{
				rejectentry->segexceeded = localrejectentry->segexceeded;
				changed                  = true;
			}
			localrejectentry->isexceeded  = false;
			localrejectentry->segexceeded = false;
		}
		else
		{
			changed = true;
			/* db objects are removed or under quota limit in the new loop */
			(void)hash_search(disk_quota_reject_map, (void *)&localrejectentry->keyitem, HASH_REMOVE, NULL);
			(void)hash_search(local_disk_quota_reject_map, (void *)&localrejectentry->keyitem, HASH_REMOVE, NULL);
		}
	}
	LWLockRelease(diskquota_locks.reject_map_lock);
	return changed;
}

/*
 * Dispatch rejectmap to segment servers.
 */
void
dispatch_rejectmap(HTAB *local_active_table_stat_map)
{
	HASH_SEQ_STATUS       hash_seq;
	GlobalRejectMapEntry *rejectmap_entry;
	ActiveTableEntry     *active_table_entry;
	int                   num_entries, count = 0;
	CdbPgResults          cdb_pgresults = {NULL, 0};
	StringInfoData        rows;
	StringInfoData        active_oids;
	StringInfoData        sql;

	initStringInfo(&rows);
	initStringInfo(&active_oids);
	initStringInfo(&sql);

	LWLockAcquire(diskquota_locks.reject_map_lock, LW_SHARED);
	num_entries = hash_get_num_entries(disk_quota_reject_map);
	hash_seq_init(&hash_seq, disk_quota_reject_map);
	while ((rejectmap_entry = hash_seq_search(&hash_seq)) != NULL)
	{
		appendStringInfo(&rows, "ROW(%d, %d, %d, %d, %s)", rejectmap_entry->keyitem.targetoid,
		                 rejectmap_entry->keyitem.databaseoid, rejectmap_entry->keyitem.tablespaceoid,
		                 rejectmap_entry->keyitem.targettype, rejectmap_entry->segexceeded ? "true" : "false");

		if (++count != num_entries) appendStringInfo(&rows, ",");
	}
	LWLockRelease(diskquota_locks.reject_map_lock);

	count       = 0;
	num_entries = hash_get_num_entries(local_active_table_stat_map);
	hash_seq_init(&hash_seq, local_active_table_stat_map);
	while ((active_table_entry = hash_seq_search(&hash_seq)) != NULL)
	{
		appendStringInfo(&active_oids, "%d", active_table_entry->reloid);

		if (++count != num_entries) appendStringInfo(&active_oids, ",");
	}

	appendStringInfo(&sql,
	                 "select diskquota.refresh_rejectmap("
	                 "ARRAY[%s]::diskquota.rejectmap_entry[], "
	                 "ARRAY[%s]::oid[])",
	                 rows.data, active_oids.data);
	CdbDispatchCommand(sql.data, DF_NONE, &cdb_pgresults);

	pfree(rows.data);
	pfree(active_oids.data);
	pfree(sql.data);
	cdbdisp_clearCdbPgResults(&cdb_pgresults);
}

static void
export_exceeded_error(GlobalRejectMapEntry *entry, bool skip_name)
{
	RejectMapEntry *rejectentry = &entry->keyitem;
	switch (rejectentry->targettype)
	{
		case NAMESPACE_QUOTA:
			ereport(ERROR, (errcode(ERRCODE_DISK_FULL), errmsg("schema's disk space quota exceeded with name: %s",
			                                                   GetNamespaceName(rejectentry->targetoid, skip_name))));
			break;
		case ROLE_QUOTA:
			ereport(ERROR, (errcode(ERRCODE_DISK_FULL), errmsg("role's disk space quota exceeded with name: %s",
			                                                   GetUserName(rejectentry->targetoid, skip_name))));
			break;
		case NAMESPACE_TABLESPACE_QUOTA:
			if (entry->segexceeded)
				ereport(ERROR, (errcode(ERRCODE_DISK_FULL),
				                errmsg("tablespace: %s, schema: %s diskquota exceeded per segment quota",
				                       GetTablespaceName(rejectentry->tablespaceoid, skip_name),
				                       GetNamespaceName(rejectentry->targetoid, skip_name))));
			else
				ereport(ERROR,
				        (errcode(ERRCODE_DISK_FULL), errmsg("tablespace: %s, schema: %s diskquota exceeded",
				                                            GetTablespaceName(rejectentry->tablespaceoid, skip_name),
				                                            GetNamespaceName(rejectentry->targetoid, skip_name))));
			break;
		case ROLE_TABLESPACE_QUOTA:
			if (entry->segexceeded)
				ereport(ERROR, (errcode(ERRCODE_DISK_FULL),
				                errmsg("tablespace: %s, role: %s diskquota exceeded per segment quota",
				                       GetTablespaceName(rejectentry->tablespaceoid, skip_name),
				                       GetUserName(rejectentry->targetoid, skip_name))));
			else
				ereport(ERROR,
				        (errcode(ERRCODE_DISK_FULL), errmsg("tablespace: %s, role: %s diskquota exceeded",
				                                            GetTablespaceName(rejectentry->tablespaceoid, skip_name),
				                                            GetUserName(rejectentry->targetoid, skip_name))));
			break;
		default:
			ereport(ERROR, (errcode(ERRCODE_DISK_FULL), errmsg("diskquota exceeded, unknown quota type")));
	}
}

bool
check_rejectmap_by_relfilenode(RelFileNode relfilenode)
{
	bool                  found;
	RejectMapEntry        keyitem;
	GlobalRejectMapEntry *entry;

	SIMPLE_FAULT_INJECTOR("check_rejectmap_by_relfilenode");

	memset(&keyitem, 0, sizeof(keyitem));
	memcpy(&keyitem.relfilenode, &relfilenode, sizeof(RelFileNode));

	LWLockAcquire(diskquota_locks.reject_map_lock, LW_SHARED);
	entry = hash_search(disk_quota_reject_map, &keyitem, HASH_FIND, &found);

	if (found && entry)
	{
		GlobalRejectMapEntry segrejectentry;
		memcpy(&segrejectentry.keyitem, &entry->auxblockinfo, sizeof(RejectMapEntry));
		segrejectentry.segexceeded = entry->segexceeded;
		LWLockRelease(diskquota_locks.reject_map_lock);

		export_exceeded_error(&segrejectentry, true /*skip_name*/);
		return false;
	}
	LWLockRelease(diskquota_locks.reject_map_lock);
	return true;
}

/*
 * Given table oid, check whether quota limit
 * of table's schema or table's owner are reached.
 * Do enforcement if quota exceeds.
 */
bool
check_rejectmap_by_reloid(Oid reloid)
{
	Oid                   ownerOid      = InvalidOid;
	Oid                   nsOid         = InvalidOid;
	Oid                   tablespaceoid = InvalidOid;
	bool                  found;
	RejectMapEntry        keyitem;
	GlobalRejectMapEntry *entry;

	bool found_rel = get_rel_owner_schema_tablespace(reloid, &ownerOid, &nsOid, &tablespaceoid);
	if (!found_rel)
	{
		return true;
	}

	LWLockAcquire(diskquota_locks.reject_map_lock, LW_SHARED);
	for (QuotaType type = 0; type < NUM_QUOTA_TYPES; ++type)
	{
		prepare_rejectmap_search_key(&keyitem, type, ownerOid, nsOid, tablespaceoid);
		entry = hash_search(disk_quota_reject_map, &keyitem, HASH_FIND, &found);
		if (found)
		{
			LWLockRelease(diskquota_locks.reject_map_lock);
			export_exceeded_error(entry, false /*skip_name*/);
			return false;
		}
	}
	LWLockRelease(diskquota_locks.reject_map_lock);
	return true;
}

/*
 * This function takes relowner, relnamespace, reltablespace as arguments,
 * prepares the searching key of the global rejectmap for us.
 */
void
prepare_rejectmap_search_key(RejectMapEntry *keyitem, QuotaType type, Oid relowner, Oid relnamespace, Oid reltablespace)
{
	Assert(keyitem != NULL);
	memset(keyitem, 0, sizeof(RejectMapEntry));
	if (type == ROLE_QUOTA || type == ROLE_TABLESPACE_QUOTA)
		keyitem->targetoid = relowner;
	else if (type == NAMESPACE_QUOTA || type == NAMESPACE_TABLESPACE_QUOTA)
		keyitem->targetoid = relnamespace;
	else if (type == TABLESPACE_QUOTA)
		keyitem->targetoid = reltablespace;
	else
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("[diskquota] unknown quota type: %d", type)));

	if (type == ROLE_TABLESPACE_QUOTA || type == NAMESPACE_TABLESPACE_QUOTA)
		keyitem->tablespaceoid = reltablespace;
	else
	{
		/* refer to add_quota_to_rejectmap */
		keyitem->tablespaceoid = InvalidOid;
	}
	keyitem->databaseoid = MyDatabaseId;
	keyitem->targettype  = type;
}

/*
 * invalidate all reject entry with a specific dbid in SHM
 */
void
invalidate_database_rejectmap(Oid dbid)
{
	RejectMapEntry *entry;
	HASH_SEQ_STATUS iter;

	LWLockAcquire(diskquota_locks.reject_map_lock, LW_EXCLUSIVE);
	hash_seq_init(&iter, disk_quota_reject_map);
	while ((entry = hash_seq_search(&iter)) != NULL)
	{
		if (entry->databaseoid == dbid || entry->relfilenode.dbNode == dbid)
		{
			hash_search(disk_quota_reject_map, entry, HASH_REMOVE, NULL);
		}
	}
	LWLockRelease(diskquota_locks.reject_map_lock);
}

/*
 * refresh_rejectmap() takes two arguments.
 * The first argument is an array of rejectmap entries on QD.
 * The second argument is an array of active relations' oid.
 *
 * The basic idea is that, we iterate over the active relations' oid, check that
 * whether the relation's owner/tablespace/namespace is in one of the rejectmap
 * entries dispatched from diskquota worker from QD. If the relation should be
 * blocked, we then add its relfilenode together with the toast, toast index,
 * appendonly, appendonly index relations' relfilenodes to the global rejectmap.
 * Note that, this UDF is called on segment servers by diskquota worker on QD and
 * the global rejectmap on segment servers is indexed by relfilenode.
 */
PG_FUNCTION_INFO_V1(refresh_rejectmap);
Datum
refresh_rejectmap(PG_FUNCTION_ARGS)
{
	ArrayType            *rejectmap_array_type  = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType            *active_oid_array_type = PG_GETARG_ARRAYTYPE_P(1);
	Oid                   rejectmap_elem_type   = ARR_ELEMTYPE(rejectmap_array_type);
	Oid                   active_oid_elem_type  = ARR_ELEMTYPE(active_oid_array_type);
	Datum                *datums;
	bool                 *nulls;
	int16                 elem_width;
	bool                  elem_type_by_val;
	char                  elem_alignment_code;
	int                   reject_array_count;
	int                   active_array_count;
	HeapTupleHeader       lt;
	bool                  segexceeded;
	GlobalRejectMapEntry *rejectmapentry;
	HASH_SEQ_STATUS       hash_seq;
	HTAB                 *local_rejectmap;
	HASHCTL               hashctl;

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to update rejectmap")));
	if (IS_QUERY_DISPATCHER())
		ereport(ERROR,
		        (errcode(ERRCODE_INTERNAL_ERROR), errmsg("\"refresh_rejectmap()\" can only be executed on QE.")));
	if (ARR_NDIM(rejectmap_array_type) > 1 || ARR_NDIM(active_oid_array_type) > 1)
		ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR), errmsg("1-dimensional array needed")));

	/*
	 * Iterate over rejectmap entries and add these entries to the local reject map
	 * on segment servers so that we are able to check whether the given relation (by oid)
	 * should be rejected in O(1) time complexity in third step.
	 */
	memset(&hashctl, 0, sizeof(hashctl));
	hashctl.keysize   = sizeof(RejectMapEntry);
	hashctl.entrysize = sizeof(GlobalRejectMapEntry);
	hashctl.hcxt      = CurrentMemoryContext;

	/*
	 * Since uncommitted relations' information and the global rejectmap entries
	 * are cached in shared memory. The memory regions are guarded by lightweight
	 * locks. In order not to hold multiple locks at the same time, We add rejectmap
	 * entries into the local_rejectmap below and then flush the content of the
	 * local_rejectmap to the global rejectmap at the end of this UDF.
	 */
	local_rejectmap =
	        diskquota_hash_create("local_rejectmap", 1024, &hashctl, HASH_ELEM | HASH_CONTEXT, DISKQUOTA_TAG_HASH);
	get_typlenbyvalalign(rejectmap_elem_type, &elem_width, &elem_type_by_val, &elem_alignment_code);
	deconstruct_array(rejectmap_array_type, rejectmap_elem_type, elem_width, elem_type_by_val, elem_alignment_code,
	                  &datums, &nulls, &reject_array_count);
	for (int i = 0; i < reject_array_count; ++i)
	{
		RejectMapEntry keyitem;
		bool           isnull;

		if (nulls[i]) continue;

		memset(&keyitem, 0, sizeof(RejectMapEntry));
		lt                    = DatumGetHeapTupleHeader(datums[i]);
		keyitem.targetoid     = DatumGetObjectId(GetAttributeByNum(lt, 1, &isnull));
		keyitem.databaseoid   = DatumGetObjectId(GetAttributeByNum(lt, 2, &isnull));
		keyitem.tablespaceoid = DatumGetObjectId(GetAttributeByNum(lt, 3, &isnull));
		keyitem.targettype    = DatumGetInt32(GetAttributeByNum(lt, 4, &isnull));
		/* rejectmap entries from QD should have the real tablespace oid */
		if ((keyitem.targettype == NAMESPACE_TABLESPACE_QUOTA || keyitem.targettype == ROLE_TABLESPACE_QUOTA))
		{
			Assert(OidIsValid(keyitem.tablespaceoid));
		}
		segexceeded = DatumGetBool(GetAttributeByNum(lt, 5, &isnull));

		rejectmapentry = hash_search(local_rejectmap, &keyitem, HASH_ENTER_NULL, NULL);
		if (rejectmapentry) rejectmapentry->segexceeded = segexceeded;
	}

	/*
	 * Thirdly, iterate over the active oid list. Check that if the relation should be blocked.
	 * If the relation should be blocked, we insert the toast, toast index, appendonly, appendonly
	 * index relations to the global reject map.
	 */
	get_typlenbyvalalign(active_oid_elem_type, &elem_width, &elem_type_by_val, &elem_alignment_code);
	deconstruct_array(active_oid_array_type, active_oid_elem_type, elem_width, elem_type_by_val, elem_alignment_code,
	                  &datums, &nulls, &active_array_count);
	for (int i = 0; i < active_array_count; ++i)
	{
		Oid       active_oid = InvalidOid;
		HeapTuple tuple;
		if (nulls[i]) continue;

		active_oid = DatumGetObjectId(datums[i]);
		if (!OidIsValid(active_oid)) continue;

		/*
		 * Since we don't take any lock on relation, check for cache
		 * invalidation messages manually to minimize risk of cache
		 * inconsistency.
		 */
		AcceptInvalidationMessages();
		tuple = SearchSysCacheCopy1(RELOID, active_oid);
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_class  form          = (Form_pg_class)GETSTRUCT(tuple);
			Oid            relnamespace  = form->relnamespace;
			Oid            reltablespace = OidIsValid(form->reltablespace) ? form->reltablespace : MyDatabaseTableSpace;
			Oid            relowner      = form->relowner;
			RejectMapEntry keyitem;
			bool           found;

			for (QuotaType type = 0; type < NUM_QUOTA_TYPES; ++type)
			{
				/* Check that if the current relation should be blocked. */
				prepare_rejectmap_search_key(&keyitem, type, relowner, relnamespace, reltablespace);
				rejectmapentry = hash_search(local_rejectmap, &keyitem, HASH_FIND, &found);
				if (found && rejectmapentry)
				{
					/*
					 * If the current relation is blocked, we should add the relfilenode
					 * of itself together with the relfilenodes of its toast relation and
					 * appendonly relations to the global reject map.
					 */
					List     *oid_list       = NIL;
					ListCell *cell           = NULL;
					Oid       toastrelid     = form->reltoastrelid;
					Oid       aosegrelid     = InvalidOid;
					Oid       aoblkdirrelid  = InvalidOid;
					Oid       aovisimaprelid = InvalidOid;
					oid_list                 = lappend_oid(oid_list, active_oid);

					/* Append toast relation and toast index to the oid_list if any. */
					if (OidIsValid(toastrelid))
					{
						oid_list = lappend_oid(oid_list, toastrelid);
						oid_list = list_concat(oid_list, diskquota_get_index_list(toastrelid));
					}

					/* Append ao auxiliary relations and their indexes to the oid_list if any. */
					diskquota_get_appendonly_aux_oid_list(active_oid, &aosegrelid, &aoblkdirrelid, &aovisimaprelid);
					if (OidIsValid(aosegrelid))
					{
						oid_list = lappend_oid(oid_list, aosegrelid);
						oid_list = list_concat(oid_list, diskquota_get_index_list(aosegrelid));
					}
					if (OidIsValid(aoblkdirrelid))
					{
						oid_list = lappend_oid(oid_list, aoblkdirrelid);
						oid_list = list_concat(oid_list, diskquota_get_index_list(aoblkdirrelid));
					}
					if (OidIsValid(aovisimaprelid))
					{
						oid_list = lappend_oid(oid_list, aovisimaprelid);
						oid_list = list_concat(oid_list, diskquota_get_index_list(aovisimaprelid));
					}

					/* Iterate over the oid_list and add their relfilenodes to the rejectmap. */
					foreach (cell, oid_list)
					{
						Oid       curr_oid   = lfirst_oid(cell);
						HeapTuple curr_tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(curr_oid));
						if (HeapTupleIsValid(curr_tuple))
						{
							Form_pg_class curr_form = (Form_pg_class)GETSTRUCT(curr_tuple);
							Oid curr_reltablespace  = OidIsValid(curr_form->reltablespace) ? curr_form->reltablespace
							                                                               : MyDatabaseTableSpace;
							RelFileNode           relfilenode = {.dbNode  = MyDatabaseId,
							                                     .relNode = curr_form->relfilenode,
							                                     .spcNode = curr_reltablespace};
							bool                  found;
							GlobalRejectMapEntry *blocked_filenode_entry;
							RejectMapEntry        blocked_filenode_keyitem;

							memset(&blocked_filenode_keyitem, 0, sizeof(RejectMapEntry));
							memcpy(&blocked_filenode_keyitem.relfilenode, &relfilenode, sizeof(RelFileNode));

							blocked_filenode_entry =
							        hash_search(local_rejectmap, &blocked_filenode_keyitem, HASH_ENTER_NULL, &found);
							if (!found && blocked_filenode_entry)
							{
								memcpy(&blocked_filenode_entry->auxblockinfo, &keyitem, sizeof(RejectMapEntry));
								blocked_filenode_entry->segexceeded = rejectmapentry->segexceeded;
							}
						}
					}
					/*
					 * The current relation may satisfy multiple blocking conditions,
					 * we only add it once.
					 */
					break;
				}
			}
		}
		else
		{
			/*
			 * We cannot fetch the relation from syscache. It may be an uncommitted relation.
			 * Let's try to fetch it from relation_cache.
			 */
			DiskQuotaRelationCacheEntry *relation_cache_entry;
			bool                         found;
			LWLockAcquire(diskquota_locks.relation_cache_lock, LW_SHARED);
			relation_cache_entry = hash_search(relation_cache, &active_oid, HASH_FIND, &found);
			/* The session of db1 should not see the table inside db2. */
			if (found && relation_cache_entry && relation_cache_entry->rnode.node.dbNode == MyDatabaseId)
			{
				Oid            relnamespace  = relation_cache_entry->namespaceoid;
				Oid            reltablespace = relation_cache_entry->rnode.node.spcNode;
				Oid            relowner      = relation_cache_entry->owneroid;
				RejectMapEntry keyitem;
				for (QuotaType type = 0; type < NUM_QUOTA_TYPES; ++type)
				{
					/* Check that if the current relation should be blocked. */
					prepare_rejectmap_search_key(&keyitem, type, relowner, relnamespace, reltablespace);
					rejectmapentry = hash_search(local_rejectmap, &keyitem, HASH_FIND, &found);

					if (found && rejectmapentry)
					{
						List     *oid_list = NIL;
						ListCell *cell     = NULL;

						/* Collect the relation oid together with its auxiliary relations' oid. */
						oid_list = lappend_oid(oid_list, active_oid);
						for (int auxoidcnt = 0; auxoidcnt < relation_cache_entry->auxrel_num; ++auxoidcnt)
							oid_list = lappend_oid(oid_list, relation_cache_entry->auxrel_oid[auxoidcnt]);

						foreach (cell, oid_list)
						{
							bool                  found;
							GlobalRejectMapEntry *blocked_filenode_entry;
							RejectMapEntry        blocked_filenode_keyitem;
							Oid                   curr_oid = lfirst_oid(cell);

							relation_cache_entry = hash_search(relation_cache, &curr_oid, HASH_FIND, &found);
							if (found && relation_cache_entry)
							{
								memset(&blocked_filenode_keyitem, 0, sizeof(RejectMapEntry));
								memcpy(&blocked_filenode_keyitem.relfilenode, &relation_cache_entry->rnode.node,
								       sizeof(RelFileNode));

								blocked_filenode_entry = hash_search(local_rejectmap, &blocked_filenode_keyitem,
								                                     HASH_ENTER_NULL, &found);
								if (!found && blocked_filenode_entry)
								{
									memcpy(&blocked_filenode_entry->auxblockinfo, &keyitem, sizeof(RejectMapEntry));
									blocked_filenode_entry->segexceeded = rejectmapentry->segexceeded;
								}
							}
						}
					}
				}
			}
			LWLockRelease(diskquota_locks.relation_cache_lock);
		}
	}

	LWLockAcquire(diskquota_locks.reject_map_lock, LW_EXCLUSIVE);

	/* Clear rejectmap entries. */
	hash_seq_init(&hash_seq, disk_quota_reject_map);
	while ((rejectmapentry = hash_seq_search(&hash_seq)) != NULL)
	{
		if (rejectmapentry->keyitem.relfilenode.dbNode != MyDatabaseId &&
		    rejectmapentry->keyitem.databaseoid != MyDatabaseId)
			continue;
		hash_search(disk_quota_reject_map, &rejectmapentry->keyitem, HASH_REMOVE, NULL);
	}

	/* Flush the content of local_rejectmap to the global rejectmap. */
	hash_seq_init(&hash_seq, local_rejectmap);
	while ((rejectmapentry = hash_seq_search(&hash_seq)) != NULL)
	{
		bool                  found;
		GlobalRejectMapEntry *new_entry;

		/*
		 * Skip soft limit reject entry. We don't perform soft-limit on segment servers, so we don't flush the
		 * rejectmap entry with a valid targetoid to the global rejectmap on segment servers.
		 */
		if (OidIsValid(rejectmapentry->keyitem.targetoid)) continue;

		new_entry = hash_search(disk_quota_reject_map, &rejectmapentry->keyitem, HASH_ENTER_NULL, &found);
		if (!found && new_entry) memcpy(new_entry, rejectmapentry, sizeof(GlobalRejectMapEntry));
	}
	LWLockRelease(diskquota_locks.reject_map_lock);

	PG_RETURN_VOID();
}

/*
 * show_rejectmap() provides developers or users to dump the rejectmap in shared
 * memory on a single server. If you want to query rejectmap on segment servers,
 * you should dispatch this query to segments.
 */
PG_FUNCTION_INFO_V1(show_rejectmap);
Datum
show_rejectmap(PG_FUNCTION_ARGS)
{
	FuncCallContext      *funcctx;
	GlobalRejectMapEntry *rejectmap_entry;
	struct RejectMapCtx
	{
		HASH_SEQ_STATUS rejectmap_seq;
		HTAB           *rejectmap;
	} * rejectmap_ctx;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc       tupdesc;
		MemoryContext   oldcontext;
		HASHCTL         hashctl;
		HASH_SEQ_STATUS hash_seq;

		/* Create a function context for cross-call persistence. */
		funcctx = SRF_FIRSTCALL_INIT();

		/* Switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		tupdesc    = DiskquotaCreateTemplateTupleDesc(9);
		TupleDescInitEntry(tupdesc, (AttrNumber)1, "target_type", TEXTOID, -1 /*typmod*/, 0 /*attdim*/);
		TupleDescInitEntry(tupdesc, (AttrNumber)2, "target_oid", OIDOID, -1 /*typmod*/, 0 /*attdim*/);
		TupleDescInitEntry(tupdesc, (AttrNumber)3, "database_oid", OIDOID, -1 /*typmod*/, 0 /*attdim*/);
		TupleDescInitEntry(tupdesc, (AttrNumber)4, "tablespace_oid", OIDOID, -1 /*typmod*/, 0 /*attdim*/);
		TupleDescInitEntry(tupdesc, (AttrNumber)5, "seg_exceeded", BOOLOID, -1 /*typmod*/, 0 /*attdim*/);
		TupleDescInitEntry(tupdesc, (AttrNumber)6, "dbnode", OIDOID, -1 /*typmod*/, 0 /*attdim*/);
		TupleDescInitEntry(tupdesc, (AttrNumber)7, "spcnode", OIDOID, -1 /*typmod*/, 0 /*attdim*/);
		TupleDescInitEntry(tupdesc, (AttrNumber)8, "relnode", OIDOID, -1 /*typmod*/, 0 /*attdim*/);
		TupleDescInitEntry(tupdesc, (AttrNumber)9, "segid", INT4OID, -1 /*typmod*/, 0 /*attdim*/);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		rejectmap_ctx = (struct RejectMapCtx *)palloc(sizeof(struct RejectMapCtx));

		/* Create a local hash table and fill it with entries from shared memory. */
		memset(&hashctl, 0, sizeof(hashctl));
		hashctl.keysize          = sizeof(RejectMapEntry);
		hashctl.entrysize        = sizeof(GlobalRejectMapEntry);
		hashctl.hcxt             = CurrentMemoryContext;
		rejectmap_ctx->rejectmap = diskquota_hash_create("rejectmap_ctx rejectmap", 1024, &hashctl,
		                                                 HASH_ELEM | HASH_CONTEXT, DISKQUOTA_TAG_HASH);

		LWLockAcquire(diskquota_locks.reject_map_lock, LW_SHARED);
		hash_seq_init(&hash_seq, disk_quota_reject_map);
		while ((rejectmap_entry = hash_seq_search(&hash_seq)) != NULL)
		{
			GlobalRejectMapEntry *local_rejectmap_entry = NULL;
			local_rejectmap_entry =
			        hash_search(rejectmap_ctx->rejectmap, &rejectmap_entry->keyitem, HASH_ENTER_NULL, NULL);
			if (local_rejectmap_entry)
			{
				memcpy(&local_rejectmap_entry->keyitem, &rejectmap_entry->keyitem, sizeof(RejectMapEntry));
				local_rejectmap_entry->segexceeded = rejectmap_entry->segexceeded;
				memcpy(&local_rejectmap_entry->auxblockinfo, &rejectmap_entry->auxblockinfo, sizeof(RejectMapEntry));
			}
		}
		LWLockRelease(diskquota_locks.reject_map_lock);

		/* Setup first calling context. */
		hash_seq_init(&(rejectmap_ctx->rejectmap_seq), rejectmap_ctx->rejectmap);
		funcctx->user_fctx = (void *)rejectmap_ctx;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx       = SRF_PERCALL_SETUP();
	rejectmap_ctx = (struct RejectMapCtx *)funcctx->user_fctx;

	while ((rejectmap_entry = hash_seq_search(&(rejectmap_ctx->rejectmap_seq))) != NULL)
	{
#define _TARGETTYPE_STR_SIZE 32
		Datum          result;
		Datum          values[9];
		bool           nulls[9];
		HeapTuple      tuple;
		RejectMapEntry keyitem;
		char           targettype_str[_TARGETTYPE_STR_SIZE];
		RelFileNode    blocked_relfilenode;

		memcpy(&blocked_relfilenode, &rejectmap_entry->keyitem.relfilenode, sizeof(RelFileNode));
		/*
		 * If the rejectmap entry is indexed by relfilenode, we dump the blocking
		 * condition from auxblockinfo.
		 */
		if (!OidIsValid(blocked_relfilenode.relNode))
			memcpy(&keyitem, &rejectmap_entry->keyitem, sizeof(keyitem));
		else
			memcpy(&keyitem, &rejectmap_entry->auxblockinfo, sizeof(keyitem));
		memset(targettype_str, 0, sizeof(targettype_str));

		switch ((QuotaType)keyitem.targettype)
		{
			case ROLE_QUOTA:
				StrNCpy(targettype_str, "ROLE_QUOTA", _TARGETTYPE_STR_SIZE);
				break;
			case NAMESPACE_QUOTA:
				StrNCpy(targettype_str, "NAMESPACE_QUOTA", _TARGETTYPE_STR_SIZE);
				break;
			case ROLE_TABLESPACE_QUOTA:
				StrNCpy(targettype_str, "ROLE_TABLESPACE_QUOTA", _TARGETTYPE_STR_SIZE);
				break;
			case NAMESPACE_TABLESPACE_QUOTA:
				StrNCpy(targettype_str, "NAMESPACE_TABLESPACE_QUOTA", _TARGETTYPE_STR_SIZE);
				break;
			default:
				StrNCpy(targettype_str, "UNKNOWN", _TARGETTYPE_STR_SIZE);
				break;
		}

		values[0] = CStringGetTextDatum(targettype_str);
		values[1] = ObjectIdGetDatum(keyitem.targetoid);
		values[2] = ObjectIdGetDatum(keyitem.databaseoid);
		values[3] = ObjectIdGetDatum(keyitem.tablespaceoid);
		values[4] = BoolGetDatum(rejectmap_entry->segexceeded);
		values[5] = ObjectIdGetDatum(blocked_relfilenode.dbNode);
		values[6] = ObjectIdGetDatum(blocked_relfilenode.spcNode);
		values[7] = ObjectIdGetDatum(blocked_relfilenode.relNode);
		values[8] = Int32GetDatum(GpIdentity.segindex);

		memset(nulls, false, sizeof(nulls));
		tuple  = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}

	SRF_RETURN_DONE(funcctx);
}

/* -------------------------------------------------------------------------
 *
 * rejectmap.h
 *
 * Copyright (c) 2020-Present VMware, Inc. or its affiliates
 *
 * IDENTIFICATION
 *		diskquota/rejectmap.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef REJECTMAP_H
#define REJECTMAP

#include "quota_config.h"

/* cluster level max size of rejectmap */
#define MAX_DISK_QUOTA_REJECT_ENTRIES (1024 * 1024)
/* cluster level init size of rejectmap */
#define INIT_DISK_QUOTA_REJECT_ENTRIES 8192
/* per database level max size of rejectmap */
#define MAX_LOCAL_DISK_QUOTA_REJECT_ENTRIES 8192
/* Number of attributes in quota configuration records. */

/* global rejectmap for which exceed their quota limit */
typedef struct RejectMapEntry
{
	Oid    targetoid;
	Oid    databaseoid;
	Oid    tablespaceoid;
	uint32 targettype;
	/*
	 * TODO refactor this data structure
	 * QD index the rejectmap by (targetoid, databaseoid, tablespaceoid, targettype).
	 * QE index the rejectmap by (relfilenode).
	 */
	RelFileNode relfilenode;
} RejectMapEntry;

typedef struct GlobalRejectMapEntry
{
	RejectMapEntry keyitem;
	bool           segexceeded;
	/*
	 * When the quota limit is exceeded on segment servers,
	 * we need an extra auxiliary field to preserve the quota
	 * limitation information for error message on segment
	 * servers, e.g., targettype, targetoid. This field is
	 * useful on segment servers.
	 */
	RejectMapEntry auxblockinfo;
} GlobalRejectMapEntry;

/* local rejectmap for which exceed their quota limit */
typedef struct LocalRejectMapEntry
{
	RejectMapEntry keyitem;
	bool           isexceeded;
	bool           segexceeded;
} LocalRejectMapEntry;

extern void init_shm_worker_rejectmap(void);
extern void init_local_reject_map(uint32 id);
extern Size diskquota_rejectmap_shmem_size(void);
extern Size diskquota_local_rejectmap_shmem_size(void);
extern void vacuum_local_reject_map(uint32 id);

extern void add_quota_to_rejectmap(QuotaType type, Oid targetOid, Oid tablespaceoid, bool segexceeded);
extern bool flush_local_reject_map(void);
extern void dispatch_rejectmap(HTAB *local_active_table_stat_map);
extern bool check_rejectmap_by_relfilenode(RelFileNode relfilenode);
extern void prepare_rejectmap_search_key(RejectMapEntry *keyitem, QuotaType type, Oid relowner, Oid relnamespace,
                                         Oid reltablespace);
extern bool check_rejectmap_by_reloid(Oid reloid);
extern void invalidate_database_rejectmap(Oid dbid);

#endif

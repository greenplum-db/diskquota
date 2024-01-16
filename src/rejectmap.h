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
#define REJECTMAP_H

#include "quota_config.h"

/* cluster level max size of rejectmap */
#define MAX_DISK_QUOTA_REJECT_ENTRIES (1024 * 1024)
/* cluster level init size of rejectmap */
#define INIT_DISK_QUOTA_REJECT_ENTRIES 8192
/* per database level max size of rejectmap */
#define MAX_LOCAL_DISK_QUOTA_REJECT_ENTRIES 8192
/* Number of attributes in quota configuration records. */

/*
 * QD index the rejectmap by (type, namespaceoid, owneroid, databaseoid, tablespaceoid).
 * QE index the rejectmap by (type, relfilenode, databaseoid).
 */
typedef struct RejectMapEntry
{
	QuotaType type;
	Oid       namespaceoid;
	Oid       owneroid;
	Oid       databaseoid;
	Oid       tablespaceoid;
	Oid       relfilenode;
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

/* center worker function */
extern void  init_shm_worker_rejectmap(void);
extern HTAB *create_reject_map(const char *name);
extern Size  diskquota_rejectmap_shmem_size(void);
extern void  vacuum_reject_map(HTAB *reject_map);

extern void add_quota_to_rejectmap(HTAB *reject_map, QuotaType type, Oid namespaceoid, Oid owneroid, Oid tablespaceoid,
                                   Oid databaseoid, bool segexceeded);
extern void clean_expired_reject_entry(HTAB *reject_map);
extern void reset_reject_map(HTAB *reject_map);
extern void invalidate_database_rejectmap(Oid dbid);

/* bgworker function */
extern void update_global_reject_map(LocalRejectMapEntry *entry);
extern void dispatch_rejectmap(HTAB *local_active_table_stat_map);

extern bool check_rejectmap_by_relfilenode(RelFileNode relfilenode);
extern bool check_rejectmap_by_reloid(Oid reloid);

#endif

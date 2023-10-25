/* -------------------------------------------------------------------------
 *
 * gp_activetable.h
 *
 * Copyright (c) 2018-2020 Pivotal Software, Inc.
 * Copyright (c) 2020-Present VMware, Inc. or its affiliates
 *
 * IDENTIFICATION
 *		diskquota/gp_activetable.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef ACTIVE_TABLE_H
#define ACTIVE_TABLE_H

#include "c.h"
#include "utils/hsearch.h"

/* Cache to detect the active table list */
typedef struct ActiveTableFileEntry
{
	Oid dbid;
	Oid relfilenode;
	Oid tablespaceoid;
} ActiveTableFileEntry;

typedef struct ActiveTableEntryKey
{
	Oid reloid;
	int segid;
} ActiveTableEntryKey;

typedef struct ActiveTableEntry
{
	Oid  reloid;
	int  segid;
	Size tablesize;
} ActiveTableEntry;

extern Size  active_table_shmem_size(void);
extern HTAB *gp_fetch_active_tables(bool force);
extern void  init_active_table_hook(void);
extern void  init_shm_worker_active_tables(void);
extern HTAB *get_active_tables_stats(ArrayType *array);
extern HTAB *get_active_tables_oid(void);

extern HTAB *active_tables_map;
extern HTAB *monitored_dbid_cache;
extern HTAB *altered_reloid_cache;

#ifndef atooid
#define atooid(x) ((Oid)strtoul((x), NULL, 10))
#endif

#endif

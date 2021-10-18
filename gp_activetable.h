#ifndef ACTIVE_TABLE_H
#define ACTIVE_TABLE_H

#include "storage/lwlock.h"
#include "diskquota.h"

/* Cache to detect the active table list */
typedef struct DiskQuotaActiveTableFileEntry
{
	Oid			dbid;
	Oid			relfilenode;
	Oid			tablespaceoid;
}	DiskQuotaActiveTableFileEntry;

typedef struct DiskQuotaRelidCacheEntry
{
	RelFileNode	relfilenode;
	Oid			relid;
}	DiskQuotaRelidCacheEntry;

typedef struct DiskQuotaRelationEntry
{
	Oid			relid;
	Oid			primary_table_oid;
}	DiskQuotaRelationEntry;

typedef struct TableEntryKey
{
	Oid		reloid;
	int		segid;
}			TableEntryKey;

typedef struct DiskQuotaActiveTableEntry
{
	Oid		reloid;
	int		segid;
	Size		tablesize;
}			DiskQuotaActiveTableEntry;

typedef struct DiskQuotaPgClassCacheEntry
{
	Oid 				reloid;
	FormData_pg_class 	pg_class_data;
	RelFileNode			relfilenode;
}		DiskQuotaPgClassCacheEntry;

extern HTAB *gp_fetch_active_tables(bool force);
extern void init_active_table_hook(void);
extern void init_shm_worker_active_tables(void);
extern void init_lock_active_tables(void);

extern HTAB *active_tables_map;
extern HTAB *monitoring_dbid_cache;
extern HTAB *relation_map;
extern HTAB	*pg_class_cache;
extern HTAB	*relid_cache;

#define atooid(x)  ((Oid) strtoul((x), NULL, 10))

#endif

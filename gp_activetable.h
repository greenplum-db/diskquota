#ifndef ACTIVE_TABLE_H
#define ACTIVE_TABLE_H

#include "access/xact.h"
#include "storage/lwlock.h"
#include "storage/relfilenode.h"
#include "diskquota.h"

/* Cache to detect the active table list */
typedef struct DiskQuotaActiveTableFileEntry
{
	Oid			dbid;
	Oid			relfilenode;
	Oid			tablespaceoid;
}			DiskQuotaActiveTableFileEntry;

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

typedef struct RelationMapEntry
{
	Oid				reloid;
	char			relkind;
	Oid				primary_reloid;
	RelFileNode		relfilenode;
	Oid				relowner;
	Oid				relnamespace;
	Oid				reltablespace;
} RelationMapEntry;

extern HTAB *gp_fetch_active_tables(bool force);
extern void init_active_table_hook(void);
extern void init_shm_worker_active_tables(void);
extern void init_lock_active_tables(void);
extern Size	calculate_relation_size(Oid reloid);
extern Size calculate_uncommitted_relation_size(Oid reloid);

extern HTAB *active_tables_map;
extern HTAB *monitoring_dbid_cache;
extern HTAB *relation_map;

#define atooid(x)  ((Oid) strtoul((x), NULL, 10))

#endif

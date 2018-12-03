#ifndef ACTIVE_TABLE_H
#define ACTIVE_TABLE_H

#include "diskquota.h"

/* Type of status of returned active table objects*/
typedef enum
{
	TABLE_COMMIT_CHANGE = 0,
	TABLE_COMMIT_CREATE = 1,
	TABLE_COMMIT_DELETE = 2,
	TABLE_IN_TRANSX_CHANGE = 3,
	TABLE_IN_TRANSX_CREATE = 4,
	TABLE_IN_TRANSX_DELETE = 5,
	TABLE_NOT_FOUND = 6,
	TABLE_UNKNOWN = 99,
} ATStatus;

typedef enum
{
	AT_CREATE = 1,
	AT_EXTEND = 2,
	AT_TRUNCATE = 3,
	AT_UNLINK = 4,
} ActiveType;

/* Cache to detect the active table list */
typedef struct DiskQuotaActiveTableFileEntry
{
	RelFileNode     node;
	Oid             inXnamespace;
	Oid             inXowner;
	ATStatus        tablestatus;
	bool			ispushedback;

} DiskQuotaActiveTableFileEntry;

typedef struct DiskQuotaActiveTableEntry
{
	RelFileNode     node;
	int64           tablesize;
	Oid             namespace;
	Oid             owner;
	ActiveType      type;
} DiskQuotaActiveTableEntry;


extern HTAB* pg_fetch_active_tables(bool);
extern void init_active_table_hook(void);
extern void init_shm_worker_active_tables(void);
extern void init_lock_active_tables(void);

extern HTAB *active_tables_map;
#endif

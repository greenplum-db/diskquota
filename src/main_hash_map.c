#include "postgres.h"

#include "utils/hsearch.h"

#include "diskquota.h"
#include "main_hash_map.h"
#include "table_size.h"
#include "quota.h"

HTAB *
init_main_hash_map(void)
{
	HASHCTL ctl;
	HTAB   *main_map;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize   = NAMEDATALEN;
	ctl.entrysize = sizeof(HashMap);
	ctl.hcxt      = TopMemoryContext;
	main_map      = diskquota_hash_create("diskquota center worker main map", 1024, &ctl, HASH_ELEM | HASH_CONTEXT,
	                                      DISKQUOTA_TAG_HASH);
}

HashMap *
search_main_hash_map(HTAB *main_map, HashMapType type, Oid dbid)
{
	StringInfoData name;
	HashMap       *entry;
	bool           found;

	initStringInfo(&name);

	switch (type)
	{
		case TABLE_SIZE_MAP:
			appendStringInfo(&name, "%s_%d", TableSizeMapNamePrefix, dbid);
			break;
		case QUOTA_INFO_MAP:
			appendStringInfo(&name, "%s_%d", QuotaInfoMapNamePrefix, dbid);
			break;
		default:
			elog(ERROR, "incorrect hash map type: %d", type);
			break;
	}

	/* find the table size map related to the current database */
	entry = hash_search(main_map, name.data, HASH_ENTER, &found);
	if (!found)
	{
		switch (type)
		{
			case TABLE_SIZE_MAP:
				// TODO: enable in the next commit
				// entry->map = create_table_size_map(name.data);
				break;
			case QUOTA_INFO_MAP:
				// TODO: enable in the next commit
				// entry->map = create_quota_info_map(name.data);
				break;
			default:
				elog(ERROR, "incorrect hash map type: %d", type);
				break;
		}
	}

	pfree(name.data);
	return entry;
}

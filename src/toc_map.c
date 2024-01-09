#include "postgres.h"

#include "utils/hsearch.h"

#include "diskquota.h"
#include "toc_map.h"
#include "table_size.h"
#include "quota.h"

HTAB *
init_toc_map(void)
{
	HASHCTL ctl;
	HTAB   *toc_map;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize   = NAMEDATALEN;
	ctl.entrysize = sizeof(HashMap);
	ctl.hcxt      = TopMemoryContext;
	toc_map       = diskquota_hash_create("diskquota center worker TOC map", 1024, &ctl, HASH_ELEM | HASH_CONTEXT,
	                                      DISKQUOTA_TAG_HASH);

	return toc_map;
}

HashMap *
search_toc_map(HTAB *toc_map, HashMapType type, Oid dbid)
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
	entry = hash_search(toc_map, name.data, HASH_ENTER, &found);
	if (!found)
	{
		switch (type)
		{
			case TABLE_SIZE_MAP:
				entry->map = create_table_size_map(name.data);
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

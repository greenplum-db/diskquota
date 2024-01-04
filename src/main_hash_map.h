#ifndef MAIN_HASH_MAP
#define MAIN HASH_MAP

#define TableSizeMapNamePrefix "TableSizeMap"
#define QuotaInfoMapNamePrefix "QuotaInfoMap"

typedef enum
{
	TABLE_SIZE_MAP,
	QUOTA_INFO_MAP,
} HashMapType;

typedef struct HashMap
{
	char  name[NAMEDATALEN];
	HTAB *map;
} HashMap;

extern HTAB    *init_main_hash_map(void);
extern HashMap *search_main_hash_map(HTAB *main_map, HashMapType type, Oid dbid);

#endif

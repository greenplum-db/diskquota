#include "postgres.h"

#include "diskquota.h"
#include "quota_config.h"
#include "config_parse.h"

static void
init_cjson_hook()
{
	cJSON_Hooks hooks;
	hooks.malloc_fn = palloc;
	hooks.free_fn   = pfree;
	cJSON_InitHooks(&hooks);
}

static char *
JSON_get_version(cJSON *head)
{
	cJSON *item = cJSON_GetObjectItem(head, "version");
	if (cJSON_IsString(item)) return cJSON_GetStringValue(item);
	return NULL;
}

static cJSON *
JSON_get_quota_list(cJSON *head)
{
	cJSON *item = cJSON_GetObjectItem(head, "quota_list");
	if (cJSON_IsArray(item)) return item;
	return NULL;
}

static QuotaType
JSON_get_quota_type(cJSON *head, const char *key)
{
	cJSON *item = cJSON_GetObjectItem(head, key);
	if (cJSON_IsNumber(item)) return (QuotaType)cJSON_GetNumberValue(item);
	return NUM_QUOTA_TYPES;
}

static Oid
JSON_get_oid(cJSON *head, const char *key)
{
	cJSON *item = cJSON_GetObjectItem(head, key);
	if (cJSON_IsNumber(item)) return (Oid)cJSON_GetNumberValue(item);
	return InvalidOid;
}

static int64
JSON_get_int64(cJSON *head, const char *key)
{
	cJSON *item = cJSON_GetObjectItem(head, key);
	if (cJSON_IsNumber(item)) return (int64)cJSON_GetNumberValue(item);
	return INVALID_QUOTA;
}

static float4
JSON_get_float4(cJSON *head, const char *key)
{
	cJSON *item = cJSON_GetObjectItem(head, key);
	if (cJSON_IsNumber(item)) return (float4)cJSON_GetNumberValue(item);
	return INVALID_SEGRATIO;
}

static void
do_parse_quota_config(Oid db_oid, cJSON *head, QuotaConfig *config)
{
	memset(config, 0, sizeof(QuotaConfig));
	config->db_oid     = db_oid;
	config->quota_type = JSON_get_quota_type(head, "quota_type");

	switch (config->quota_type)
	{
		case NAMESPACE_QUOTA:
			config->namespace_oid  = JSON_get_oid(head, "namespace_oid");
			config->quota_limit_mb = JSON_get_int64(head, "quota_limit_mb");
			break;
		case ROLE_QUOTA:
			config->owner_oid      = JSON_get_oid(head, "owner_oid");
			config->quota_limit_mb = JSON_get_int64(head, "quota_limit_mb");
			break;
		case NAMESPACE_TABLESPACE_QUOTA:
			config->namespace_oid  = JSON_get_oid(head, "namespace_oid");
			config->tablespace_oid = JSON_get_oid(head, "tablespace_oid");
			config->quota_limit_mb = JSON_get_int64(head, "quota_limit_mb");
			break;
		case ROLE_TABLESPACE_QUOTA:
			config->owner_oid      = JSON_get_oid(head, "owner_oid");
			config->tablespace_oid = JSON_get_oid(head, "tablespace_oid");
			config->quota_limit_mb = JSON_get_int64(head, "quota_limit_mb");
			break;
		case TABLESPACE_QUOTA:
			config->tablespace_oid = JSON_get_oid(head, "tablespace_oid");
			config->segratio       = JSON_get_float4(head, "segratio");
			break;
		default:
			ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
			                errmsg("[diskquota] quota type is incorrect for: %d", config->quota_type)));
			break;
	}
}

static cJSON *
do_construct_quota_config(QuotaConfig *config)
{
	cJSON *head = cJSON_CreateObject();
	cJSON_AddNumberToObject(head, "quota_type", config->quota_type);

	switch (config->quota_type)
	{
		case NAMESPACE_QUOTA:
			cJSON_AddNumberToObject(head, "namespace_oid", config->namespace_oid);
			cJSON_AddNumberToObject(head, "quota_limit_mb", config->quota_limit_mb);
			break;
		case ROLE_QUOTA:
			cJSON_AddNumberToObject(head, "owner_oid", config->owner_oid);
			cJSON_AddNumberToObject(head, "quota_limit_mb", config->quota_limit_mb);
			break;
		case NAMESPACE_TABLESPACE_QUOTA:
			cJSON_AddNumberToObject(head, "namespace_oid", config->namespace_oid);
			cJSON_AddNumberToObject(head, "tablespace_oid", config->tablespace_oid);
			cJSON_AddNumberToObject(head, "quota_limit_mb", config->quota_limit_mb);
			break;
		case ROLE_TABLESPACE_QUOTA:
			cJSON_AddNumberToObject(head, "owner_oid", config->owner_oid);
			cJSON_AddNumberToObject(head, "tablespace_oid", config->tablespace_oid);
			cJSON_AddNumberToObject(head, "quota_limit_mb", config->quota_limit_mb);
			break;
		case TABLESPACE_QUOTA:
			cJSON_AddNumberToObject(head, "tablespace_oid", config->tablespace_oid);
			cJSON_AddNumberToObject(head, "segratio", config->segratio);
			break;
		default:
			ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
			                errmsg("[diskquota] quota type is incorrect for: %d", config->quota_type)));
			break;
	}
	return head;
}

/* construct quota config hashmap by config_str */
void
JSON_parse_quota_config(Oid db_oid, const char *config_str, HTAB *quota_config_map)
{
	cJSON         *head;
	cJSON         *quota_list;
	cJSON         *quota_item;
	int            quota_list_size;
	QuotaConfigKey key = {0};
	QuotaConfig    config;
	QuotaConfig   *entry;
	char          *version;
	int            i;

	if (config_str == NULL) return;

	init_cjson_hook();
	head            = cJSON_Parse(config_str);
	version         = JSON_get_version(head);
	quota_list      = JSON_get_quota_list(head);
	quota_list_size = cJSON_GetArraySize(quota_list);

	for (i = 0; i < quota_list_size; i++)
	{
		quota_item = cJSON_GetArrayItem(quota_list, i);
		if (strcmp(version, "diskquota-3.0") == 0) do_parse_quota_config(db_oid, quota_item, &config);
		memcpy(&key, &config, sizeof(QuotaConfigKey));
		entry = hash_search(quota_config_map, &key, HASH_ENTER_NULL, NULL);
		memcpy(entry, &config, sizeof(QuotaConfig));
	}
}

char *
JSON_construct_quota_config(Oid db_oid, HTAB *quota_config_map)
{
	cJSON	      *head;
	cJSON	      *quota_list;
	cJSON	      *quota_item;
	HASH_SEQ_STATUS iter;
	QuotaConfig    *entry;

	if (quota_config_map == NULL) return NULL;

	init_cjson_hook();
	head = cJSON_CreateObject();
	cJSON_AddStringToObject(head, "version", "diskquota-3.0");
	quota_list = cJSON_AddArrayToObject(head, "quota_list");

	hash_seq_init(&iter, quota_config_map);
	while ((entry = (QuotaConfig *)hash_seq_search(&iter)) != NULL)
	{
		quota_item = do_construct_quota_config(entry);
		cJSON_AddItemToArray(quota_list, quota_item);
	}

	return cJSON_Print(head);
}

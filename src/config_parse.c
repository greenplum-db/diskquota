/*-------------------------------------------------------------------------
 *
 * config_parse.c
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/config_parse.c
 *
 *-------------------------------------------------------------------------
 */

#ifdef DISKQUOTA_UNIT_TEST
#include "gp_mock.h"
#else
#include "postgres.h"
#include "c.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/hsearch.h"
#endif

#include "quota_config.h"
#include "config_parse.h"

#include <math.h>

void
init_cjson_hook(malloc_fn mfn, free_fn ffn)
{
	cJSON_Hooks hooks;
	hooks.malloc_fn = mfn;
	hooks.free_fn   = ffn;
	cJSON_InitHooks(&hooks);
}

char *
JSON_get_version(cJSON *head)
{
	cJSON *item = cJSON_GetObjectItem(head, "version");
	Assert(cJSON_IsString(item));
	return cJSON_GetStringValue(item);
}

cJSON *
JSON_get_quota_list(cJSON *head)
{
	cJSON *item = cJSON_GetObjectItem(head, "quota_list");
	Assert(cJSON_IsArray(item));
	return item;
}

QuotaType
JSON_get_quota_type(cJSON *head, const char *key)
{
	cJSON *item = cJSON_GetObjectItem(head, key);
	Assert(cJSON_IsNumber(item));
	return round(cJSON_GetNumberValue(item));
}

static Oid
JSON_get_oid(cJSON *head, const char *key)
{
	cJSON *item = cJSON_GetObjectItem(head, key);
	Assert(cJSON_IsNumber(item));
	return round(cJSON_GetNumberValue(item));
}

static int64
JSON_get_int64(cJSON *head, const char *key)
{
	cJSON *item = cJSON_GetObjectItem(head, key);
	Assert(cJSON_IsNumber(item));
	return (int64)round(cJSON_GetNumberValue(item));
}

static float8
JSON_get_float4(cJSON *head, const char *key)
{
	cJSON *item = cJSON_GetObjectItem(head, key);
	Assert(cJSON_IsNumber(item));
	return (float8)cJSON_GetNumberValue(item);
}

/*
 * parse JSON struct to QuotaConfig.
 * return 0: success.
 * return -1: failed.
 */
int
do_parse_quota_config(cJSON *head, QuotaConfig *config)
{
	memset(config, 0, sizeof(QuotaConfig));
	config->quota_type = JSON_get_quota_type(head, "quota_type");

	switch (config->quota_type)
	{
		case NAMESPACE_QUOTA:
			config->keys[0]        = JSON_get_oid(head, "db_oid");
			config->keys[1]        = JSON_get_oid(head, "namespace_oid");
			config->quota_limit_mb = JSON_get_int64(head, "quota_limit_mb");
			break;
		case ROLE_QUOTA:
			config->keys[0]        = JSON_get_oid(head, "db_oid");
			config->keys[1]        = JSON_get_oid(head, "owner_oid");
			config->quota_limit_mb = JSON_get_int64(head, "quota_limit_mb");
			break;
		case NAMESPACE_TABLESPACE_QUOTA:
			config->keys[0]        = JSON_get_oid(head, "db_oid");
			config->keys[1]        = JSON_get_oid(head, "namespace_oid");
			config->keys[2]        = JSON_get_oid(head, "tablespace_oid");
			config->quota_limit_mb = JSON_get_int64(head, "quota_limit_mb");
			break;
		case ROLE_TABLESPACE_QUOTA:
			config->keys[0]        = JSON_get_oid(head, "db_oid");
			config->keys[1]        = JSON_get_oid(head, "owner_oid");
			config->keys[2]        = JSON_get_oid(head, "tablespace_oid");
			config->quota_limit_mb = JSON_get_int64(head, "quota_limit_mb");
			break;
		case TABLESPACE_QUOTA:
			config->keys[0]  = JSON_get_oid(head, "db_oid");
			config->keys[1]  = JSON_get_oid(head, "tablespace_oid");
			config->segratio = JSON_get_float4(head, "segratio");
			break;
		default:
			return -1;
	}
	return 0;
}

/*
 * construct JSON struct by QuotaConfig.
 * return NULL: failed.
 */
cJSON *
do_construct_quota_config(QuotaConfig *config)
{
	cJSON *head = cJSON_CreateObject();
	cJSON_AddNumberToObject(head, "quota_type", config->quota_type);

	switch (config->quota_type)
	{
		case NAMESPACE_QUOTA:
			cJSON_AddNumberToObject(head, "db_oid", config->keys[0]);
			cJSON_AddNumberToObject(head, "namespace_oid", config->keys[1]);
			cJSON_AddNumberToObject(head, "quota_limit_mb", config->quota_limit_mb);
			break;
		case ROLE_QUOTA:
			cJSON_AddNumberToObject(head, "db_oid", config->keys[0]);
			cJSON_AddNumberToObject(head, "owner_oid", config->keys[1]);
			cJSON_AddNumberToObject(head, "quota_limit_mb", config->quota_limit_mb);
			break;
		case NAMESPACE_TABLESPACE_QUOTA:
			cJSON_AddNumberToObject(head, "db_oid", config->keys[0]);
			cJSON_AddNumberToObject(head, "namespace_oid", config->keys[1]);
			cJSON_AddNumberToObject(head, "tablespace_oid", config->keys[2]);
			cJSON_AddNumberToObject(head, "quota_limit_mb", config->quota_limit_mb);
			break;
		case ROLE_TABLESPACE_QUOTA:
			cJSON_AddNumberToObject(head, "db_oid", config->keys[0]);
			cJSON_AddNumberToObject(head, "owner_oid", config->keys[1]);
			cJSON_AddNumberToObject(head, "tablespace_oid", config->keys[2]);
			cJSON_AddNumberToObject(head, "quota_limit_mb", config->quota_limit_mb);
			break;
		case TABLESPACE_QUOTA:
			cJSON_AddNumberToObject(head, "db_oid", config->keys[0]);
			cJSON_AddNumberToObject(head, "tablespace_oid", config->keys[1]);
			cJSON_AddNumberToObject(head, "segratio", config->segratio);
			break;
		default:
			cJSON_Delete(head);
			return NULL;
	}
	return head;
}

/* construct quota config hashmap by config_str */
void
JSON_parse_quota_config(const char *config_str, HTAB *quota_config_map)
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

	init_cjson_hook(palloc, pfree);
	head            = cJSON_Parse(config_str);
	version         = JSON_get_version(head);
	quota_list      = JSON_get_quota_list(head);
	quota_list_size = cJSON_GetArraySize(quota_list);

	for (i = 0; i < quota_list_size; i++)
	{
		quota_item = cJSON_GetArrayItem(quota_list, i);
		if (strcmp(version, "diskquota-3.0") == 0)
		{
			if (do_parse_quota_config(quota_item, &config))
			{
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
				                errmsg("[diskquota] quota type is incorrect for: %d", config.quota_type)));
			}
		}
		key.quota_type = config.quota_type;
		memcpy(key.keys, config.keys, sizeof(Oid) * MAX_QUOTA_KEY_NUM);
		entry = hash_search(quota_config_map, &key, HASH_ENTER_NULL, NULL);
		if (entry)
		{
			entry->quota_limit_mb = config.quota_limit_mb;
			entry->segratio       = config.segratio;
		}
	}
	cJSON_Delete(head);
}

char *
JSON_construct_quota_config(HTAB *quota_config_map)
{
	cJSON	      *head;
	cJSON	      *quota_list;
	cJSON	      *quota_item;
	HASH_SEQ_STATUS iter;
	QuotaConfig    *entry;

	if (quota_config_map == NULL) return NULL;

	init_cjson_hook(palloc, pfree);
	head = cJSON_CreateObject();
	cJSON_AddStringToObject(head, "version", "diskquota-3.0");
	quota_list = cJSON_AddArrayToObject(head, "quota_list");

	hash_seq_init(&iter, quota_config_map);
	while ((entry = (QuotaConfig *)hash_seq_search(&iter)) != NULL)
	{
		quota_item = do_construct_quota_config(entry);
		if (quota_item == NULL)
		{
			ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
			                errmsg("[diskquota] quota type is incorrect for: %d", entry->quota_type)));
		}
		cJSON_AddItemToArray(quota_list, quota_item);
	}

	return cJSON_Print(head);
}

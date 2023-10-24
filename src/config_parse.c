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

#include "postgres.h"

#include "diskquota.h"
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
	return roundl(cJSON_GetNumberValue(item));
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

static float4
JSON_get_float4(cJSON *head, const char *key)
{
	cJSON *item = cJSON_GetObjectItem(head, key);
	Assert(cJSON_IsNumber(item));
	return (float4)cJSON_GetNumberValue(item);
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
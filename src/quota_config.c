/*-------------------------------------------------------------------------
 *
 * quota_config.c
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/quota_config.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>

#include "catalog/pg_authid.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "commands/tablespace.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/acl.h"
#include "utils/syscache.h"

#include "quota_config.h"
#include "config_parse.h"
#include "diskquota_util.h"
#include "diskquota.h"

/* quota config function */
extern HTAB *pull_quota_config(bool *found);
static void  update_quota_config_table(QuotaConfig *config, bool need_del_quota);
static void  dump_to_quota_config_table(const char *config_json_str, bool need_update);
static bool  to_delete_quota(QuotaType type, int64 quota_limit_mb, float4 segratio);

/* JSON function */
static void  JSON_parse_quota_config(const char *config_str, HTAB *quota_config_map);
static char *JSON_construct_quota_config(HTAB *quota_config_map);

PG_FUNCTION_INFO_V1(set_schema_quota);
PG_FUNCTION_INFO_V1(set_role_quota);
PG_FUNCTION_INFO_V1(set_schema_tablespace_quota);
PG_FUNCTION_INFO_V1(set_role_tablespace_quota);
PG_FUNCTION_INFO_V1(set_tablespace_quota);

/*--------------------UDF--------------------*/
/*
 * Set disk quota limit for schema.
 */
Datum
set_schema_quota(PG_FUNCTION_ARGS)
{
	char       *nspname;
	QuotaConfig config = {0};
	bool        need_delete_quota;
	Oid         db_oid;
	Oid         namespace_oid;

	if (!superuser())
	{
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to set disk quota limit")));
	}

	db_oid                = MyDatabaseId;
	nspname               = text_to_cstring(PG_GETARG_TEXT_PP(0));
	namespace_oid         = get_oid_auto_case_convert(get_namespace_oid, nspname);
	config.keys[0]        = db_oid;
	config.keys[1]        = namespace_oid;
	config.quota_limit_mb = get_size_in_mb(text_to_cstring(PG_GETARG_TEXT_PP(1)));
	config.quota_type     = NAMESPACE_QUOTA;

	if (config.quota_limit_mb == 0)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("disk quota can not be set to 0 MB")));
	}

	need_delete_quota = to_delete_quota(config.quota_type, config.quota_limit_mb, config.segratio);

	SPI_connect();
	update_quota_config_table(&config, need_delete_quota);
	SPI_finish();
	PG_RETURN_VOID();
}

/*
 * Set disk quota limit for role.
 */
Datum
set_role_quota(PG_FUNCTION_ARGS)
{
	char       *rolname;
	QuotaConfig config = {0};
	bool        need_delete_quota;
	Oid         db_oid;
	Oid         role_oid;

	if (!superuser())
	{
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to set disk quota limit")));
	}

	db_oid                = MyDatabaseId;
	rolname               = text_to_cstring(PG_GETARG_TEXT_PP(0));
	role_oid              = get_oid_auto_case_convert(get_role_oid, rolname);
	config.keys[0]        = db_oid;
	config.keys[1]        = role_oid;
	config.quota_limit_mb = get_size_in_mb(text_to_cstring(PG_GETARG_TEXT_PP(1)));
	config.quota_type     = ROLE_QUOTA;

	if (config.quota_limit_mb == 0)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("disk quota can not be set to 0 MB")));
	}

	/* reject setting quota for super user, but deletion is allowed */
	if (role_oid == BOOTSTRAP_SUPERUSERID && config.quota_limit_mb >= 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		                errmsg("Can not set disk quota for system owner: %s", rolname)));

	need_delete_quota = to_delete_quota(config.quota_type, config.quota_limit_mb, config.segratio);

	SPI_connect();
	update_quota_config_table(&config, need_delete_quota);
	SPI_finish();
	PG_RETURN_VOID();
}

/*
 * Set disk quota limit for schema and tablepace.
 */
Datum
set_schema_tablespace_quota(PG_FUNCTION_ARGS)
{
	char       *nspname;
	char       *spcname;
	QuotaConfig config = {0};
	bool        need_delete_quota;
	Oid         db_oid;
	Oid         namespace_oid;
	Oid         tablespace_oid;

	if (!superuser())
	{
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to set disk quota limit")));
	}

	db_oid                = MyDatabaseId;
	nspname               = text_to_cstring(PG_GETARG_TEXT_PP(0));
	namespace_oid         = get_oid_auto_case_convert(get_namespace_oid, nspname);
	spcname               = text_to_cstring(PG_GETARG_TEXT_PP(1));
	tablespace_oid        = get_oid_auto_case_convert(get_tablespace_oid, spcname);
	config.keys[0]        = db_oid;
	config.keys[1]        = namespace_oid;
	config.keys[2]        = tablespace_oid;
	config.quota_limit_mb = get_size_in_mb(text_to_cstring(PG_GETARG_TEXT_PP(2)));
	config.quota_type     = NAMESPACE_TABLESPACE_QUOTA;

	if (config.quota_limit_mb == 0)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("disk quota can not be set to 0 MB")));
	}

	need_delete_quota = to_delete_quota(config.quota_type, config.quota_limit_mb, config.segratio);

	SPI_connect();
	update_quota_config_table(&config, need_delete_quota);
	SPI_finish();
	PG_RETURN_VOID();
}

/*
 * Set disk quota limit for role and tablepace.
 */
Datum
set_role_tablespace_quota(PG_FUNCTION_ARGS)
{
	char       *rolname;
	char       *spcname;
	QuotaConfig config = {0};
	bool        need_delete_quota;
	Oid         db_oid;
	Oid         role_oid;
	Oid         tablespace_oid;

	if (!superuser())
	{
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to set disk quota limit")));
	}

	db_oid                = MyDatabaseId;
	rolname               = text_to_cstring(PG_GETARG_TEXT_PP(0));
	role_oid              = get_oid_auto_case_convert(get_role_oid, rolname);
	spcname               = text_to_cstring(PG_GETARG_TEXT_PP(1));
	tablespace_oid        = get_oid_auto_case_convert(get_tablespace_oid, spcname);
	config.keys[0]        = db_oid;
	config.keys[1]        = role_oid;
	config.keys[2]        = tablespace_oid;
	config.quota_limit_mb = get_size_in_mb(text_to_cstring(PG_GETARG_TEXT_PP(2)));
	config.quota_type     = ROLE_TABLESPACE_QUOTA;

	if (config.quota_limit_mb == 0)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("disk quota can not be set to 0 MB")));
	}

	/* reject setting quota for super user, but deletion is allowed */
	if (role_oid == BOOTSTRAP_SUPERUSERID && config.quota_limit_mb >= 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		                errmsg("Can not set disk quota for system owner: %s", rolname)));

	need_delete_quota = to_delete_quota(config.quota_type, config.quota_limit_mb, config.segratio);

	SPI_connect();
	update_quota_config_table(&config, need_delete_quota);
	SPI_finish();
	PG_RETURN_VOID();
}

/*
 * Set disk quota limit for tablepace.
 */
Datum
set_tablespace_quota(PG_FUNCTION_ARGS)
{
	char       *spcname;
	QuotaConfig config = {0};
	bool        need_delete_quota;
	Oid         db_oid;
	Oid         tablespace_oid;

	if (!superuser())
	{
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to set disk quota limit")));
	}

	db_oid            = MyDatabaseId;
	spcname           = text_to_cstring(PG_GETARG_TEXT_PP(0));
	tablespace_oid    = get_oid_auto_case_convert(get_tablespace_oid, spcname);
	config.keys[0]    = db_oid;
	config.keys[1]    = tablespace_oid;
	config.segratio   = PG_GETARG_FLOAT4(1);
	config.quota_type = TABLESPACE_QUOTA;

	need_delete_quota = to_delete_quota(config.quota_type, config.quota_limit_mb, config.segratio);

	if (config.segratio == 0)
	{
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("per segment quota ratio can not be set to 0")));
	}

	SPI_connect();
	update_quota_config_table(&config, need_delete_quota);
	SPI_finish();
	PG_RETURN_VOID();
}

/*--------------------Quota Function--------------------*/
/* check whether quota_config should be deleted */
static bool
to_delete_quota(QuotaType type, int64 quota_limit_mb, float4 segratio)
{
	if (quota_limit_mb < 0)
		return true;
	else if (segratio < 0 && type == TABLESPACE_QUOTA)
		return true;
	return false;
}

/* pull quota config from diskquota.quota_config to hashmap. */
HTAB *
pull_quota_config(bool *found)
{
	int     ret;
	HTAB   *quota_config_map;
	HASHCTL hash_ctl;

	ret = SPI_execute("select config::text::cstring from diskquota.quota_config", true, 0);
	if (ret != SPI_OK_SELECT) elog(ERROR, "cannot select quota setting table: error code %d", ret);

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(QuotaConfigKey);
	hash_ctl.entrysize = sizeof(QuotaConfig);
	hash_ctl.hcxt      = CurrentMemoryContext;

	quota_config_map = diskquota_hash_create("quota config map", MAX_QUOTA_NUM, &hash_ctl, HASH_ELEM | HASH_CONTEXT,
	                                         DISKQUOTA_TAG_HASH);

	if (found != NULL) *found = false;

	if (SPI_processed > 0)
	{
		HeapTuple tup = SPI_tuptable->vals[0];
		Datum     dat;
		bool      isnull;
		char     *json_str;

		dat = SPI_getbinval(tup, SPI_tuptable->tupdesc, 1, &isnull);
		Assert(isnull == false);
		json_str = DatumGetCString(dat);

		if (found != NULL) *found = true;

		/* fill quota config hashmap by quota config json string */
		JSON_parse_quota_config(json_str, quota_config_map);
	}

	return quota_config_map;
}

/* update quota config */
static void
update_quota_config_table(QuotaConfig *config, bool need_del_quota)
{
	QuotaConfigKey key = {0};
	QuotaConfig   *entry;
	HTAB          *quota_config_map;
	char          *new_quota_config_json_str;
	bool           need_update;

	/* If a row exists, an update is needed. */
	quota_config_map = pull_quota_config(&need_update);

	memcpy(&key, config, sizeof(QuotaConfigKey));
	/* remove quota config */
	if (need_del_quota) hash_search(quota_config_map, &key, HASH_REMOVE, NULL);
	/* update/insert quota config */
	else
	{
		entry = hash_search(quota_config_map, &key, HASH_ENTER_NULL, NULL);
		memcpy(entry, config, sizeof(QuotaConfig));
	}
	/* construct new quota config json string */
	new_quota_config_json_str = JSON_construct_quota_config(quota_config_map);
	/* dump quota config to diskquota.quota_config */
	dump_to_quota_config_table(new_quota_config_json_str, need_update);
	hash_destroy(quota_config_map);
}

/* persist quota config as json to diskquota.quota_config */
static void
dump_to_quota_config_table(const char *config_json_str, bool need_update)
{
	int ret;

	if (need_update)
	{
		ret = SPI_execute_with_args("update diskquota.quota_config set config = $1::text::jsonb", 1,
		                            (Oid[]){
		                                    CSTRINGOID,
		                            },
		                            (Datum[]){
		                                    CStringGetDatum(config_json_str),
		                            },
		                            NULL, false, 0);
		if (ret != SPI_OK_UPDATE)
			ereport(ERROR, (errmsg("cannot update quota setting table, reason: %s.", SPI_result_code_string(ret))));
	}
	else
	{
		ret = SPI_execute_with_args("insert into diskquota.quota_config values($1::text::jsonb)", 1,
		                            (Oid[]){
		                                    CSTRINGOID,
		                            },
		                            (Datum[]){
		                                    CStringGetDatum(config_json_str),
		                            },
		                            NULL, false, 0);
		if (ret != SPI_OK_INSERT)
			ereport(ERROR,
			        (errmsg("cannot insert into quota setting table, reason: %s.", SPI_result_code_string(ret))));
	}
}

/*--------------------JSON--------------------*/
/* construct quota config hashmap by config_str */
static void
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

static char *
JSON_construct_quota_config(HTAB *quota_config_map)
{
	cJSON	      *head;
	cJSON	      *quota_list;
	cJSON	      *quota_item;
	HASH_SEQ_STATUS iter;
	QuotaConfig    *entry;
	char           *ret_str;

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

	ret_str = cJSON_Print(head);
	cJSON_Delete(head);
	return ret_str;
}


#include "postgres.h"

#include <sys/stat.h>

#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "commands/tablespace.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/acl.h"

#include "diskquota.h"
#include "quota_config.h"
#include "config_parse.h"

/* quota config function */
static void  update_quota_config_table(QuotaConfig *config, bool need_del_quota);
static void  dump_to_quota_config_table(Oid db_oid, const char *config_json_str, bool need_update);
static Datum __get_oid_auto_case_convert(Oid (*f)(const char *name, bool missing_ok), const char *name);
static bool  to_delete_quota(QuotaType type, int64 quota_limit_mb, float4 segratio);

PG_FUNCTION_INFO_V1(set_schema_quota_3);
PG_FUNCTION_INFO_V1(set_role_quota_3);
PG_FUNCTION_INFO_V1(set_schema_tablespace_quota_3);
PG_FUNCTION_INFO_V1(set_role_tablespace_quota_3);
PG_FUNCTION_INFO_V1(set_tablespace_quota_3);

/*
 * Set disk quota limit for schema.
 */
Datum
set_schema_quota_3(PG_FUNCTION_ARGS)
{
	char       *nspname;
	QuotaConfig config = {0};
	bool        need_delete_quota;

	if (!superuser())
	{
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to set disk quota limit")));
	}

	config.db_oid         = PG_GETARG_OID(0);
	nspname               = text_to_cstring(PG_GETARG_TEXT_PP(1));
	config.namespace_oid  = __get_oid_auto_case_convert(get_namespace_oid, nspname);
	config.quota_limit_mb = PG_GETARG_INT64(2);
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
set_role_quota_3(PG_FUNCTION_ARGS)
{
	char       *rolname;
	QuotaConfig config = {0};
	bool        need_delete_quota;

	if (!superuser())
	{
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to set disk quota limit")));
	}

	config.db_oid         = PG_GETARG_OID(0);
	rolname               = text_to_cstring(PG_GETARG_TEXT_PP(1));
	config.owner_oid      = __get_oid_auto_case_convert(get_role_oid, rolname);
	config.quota_limit_mb = PG_GETARG_INT64(2);
	config.quota_type     = ROLE_QUOTA;

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
 * Set disk quota limit for schema and tablepace.
 */
Datum
set_schema_tablespace_quota_3(PG_FUNCTION_ARGS)
{
	char       *nspname;
	char       *spcname;
	QuotaConfig config = {0};
	bool        need_delete_quota;

	if (!superuser())
	{
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to set disk quota limit")));
	}

	config.db_oid         = PG_GETARG_OID(0);
	nspname               = text_to_cstring(PG_GETARG_TEXT_PP(1));
	config.namespace_oid  = __get_oid_auto_case_convert(get_namespace_oid, nspname);
	spcname               = text_to_cstring(PG_GETARG_TEXT_PP(2));
	config.tablespace_oid = __get_oid_auto_case_convert(get_tablespace_oid, spcname);
	config.quota_limit_mb = PG_GETARG_INT64(3);
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
set_role_tablespace_quota_3(PG_FUNCTION_ARGS)
{
	char       *rolname;
	char       *spcname;
	QuotaConfig config = {0};
	bool        need_delete_quota;

	if (!superuser())
	{
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to set disk quota limit")));
	}

	config.db_oid         = PG_GETARG_OID(0);
	rolname               = text_to_cstring(PG_GETARG_TEXT_PP(1));
	config.owner_oid      = __get_oid_auto_case_convert(get_role_oid, rolname);
	spcname               = text_to_cstring(PG_GETARG_TEXT_PP(2));
	config.tablespace_oid = __get_oid_auto_case_convert(get_tablespace_oid, spcname);
	config.quota_limit_mb = PG_GETARG_INT64(3);
	config.quota_type     = ROLE_TABLESPACE_QUOTA;

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
 * Set disk quota limit for tablepace.
 */
Datum
set_tablespace_quota_3(PG_FUNCTION_ARGS)
{
	char       *spcname;
	QuotaConfig config = {0};
	bool        need_delete_quota;

	if (!superuser())
	{
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to set disk quota limit")));
	}

	config.db_oid         = PG_GETARG_OID(0);
	spcname               = text_to_cstring(PG_GETARG_TEXT_PP(1));
	config.tablespace_oid = __get_oid_auto_case_convert(get_tablespace_oid, spcname);
	config.segratio       = PG_GETARG_FLOAT4(2);
	config.quota_type     = TABLESPACE_QUOTA;

	need_delete_quota = to_delete_quota(config.quota_type, config.quota_limit_mb, config.segratio);

	SPI_connect();
	update_quota_config_table(&config, need_delete_quota);
	SPI_finish();
	PG_RETURN_VOID();
}

static Datum
__get_oid_auto_case_convert(Oid (*f)(const char *name, bool missing_ok), const char *name)
{
	char *b   = NULL;
	int   l   = strlen(name);
	Oid   ret = InvalidOid;

	if (l > 2 && name[0] == '"' && name[l - 1] == '"')
	{
		// object name wrapped by '"'. eg: "foo"
		b = palloc(l);
		StrNCpy(b, name + 1, l - 1); // trim the '"'. unlike strncpy, StrNCpy will ensure b[l-1] = '\0'
	}
	else
	{
		// lower the object name if not wrapped by '"'
		b = str_tolower(name, strlen(name), DEFAULT_COLLATION_OID);
	}

	ret = f(b, false);

	pfree(b);
	return ret;
}

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

/* update quota config */
static void
update_quota_config_table(QuotaConfig *config, bool need_del_quota)
{
	QuotaConfigKey key = {0};
	QuotaConfig   *entry;
	HTAB          *quota_config_map;
	char          *new_quota_config_json_str;
	int            ret;
	bool           need_update = false;
	HASHCTL        hash_ctl;

	ret = SPI_execute_with_args("select config::text::cstring from diskquota.quota_config_3 where databaseOid = $1", 1,
	                            (Oid[]){
	                                    OIDOID,
	                            },
	                            (Datum[]){
	                                    ObjectIdGetDatum(config->db_oid),
	                            },
	                            NULL, true, 0);
	if (ret != SPI_OK_SELECT) elog(ERROR, "cannot select quota setting table: error code %d", ret);

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(QuotaConfigKey);
	hash_ctl.entrysize = sizeof(QuotaConfig);
	hash_ctl.hash      = tag_hash;

	quota_config_map =
	        hash_create("quota config map", MAX_QUOTA_NUM, &hash_ctl, HASH_ELEM | HASH_CONTEXT | HASH_FUNCTION);

	if (SPI_processed > 0)
	{
		HeapTuple tup = SPI_tuptable->vals[0];
		Datum     dat;
		bool      isnull;
		char     *json_str;

		need_update = true;
		dat         = SPI_getbinval(tup, SPI_tuptable->tupdesc, 1, &isnull);
		Assert(isnull == false);
		json_str = DatumGetCString(dat);

		/* fill quota config hashmap by quota config json string */
		JSON_parse_quota_config(config->db_oid, json_str, quota_config_map);
	}

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
	new_quota_config_json_str = JSON_construct_quota_config(config->db_oid, quota_config_map);
	/* dump quota config to diskquota.quota_config */
	dump_to_quota_config_table(config->db_oid, new_quota_config_json_str, need_update);
}

/* persist quota config as json to diskquota.quota_config */
static void
dump_to_quota_config_table(Oid db_oid, const char *config_json_str, bool need_update)
{
	int ret;
	elog(WARNING, "config_json_str: %s", config_json_str);

	if (need_update)
	{
		ret = SPI_execute_with_args(
		        "update diskquota.quota_config_3 set config = $1::text::jsonb where databaseOid = $2", 2,
		        (Oid[]){
		                CSTRINGOID,
		                OIDOID,
		        },
		        (Datum[]){
		                CStringGetDatum(config_json_str),
		                ObjectIdGetDatum(db_oid),
		        },
		        NULL, false, 0);
		if (ret != SPI_OK_UPDATE) elog(ERROR, "cannot update quota setting table, error code %d", ret);
	}
	else
	{
		ret = SPI_execute_with_args("insert into diskquota.quota_config_3 values($1, $2::text::jsonb)", 2,
		                            (Oid[]){
		                                    OIDOID,
		                                    CSTRINGOID,
		                            },
		                            (Datum[]){
		                                    ObjectIdGetDatum(db_oid),
		                                    CStringGetDatum(config_json_str),
		                            },
		                            NULL, false, 0);
		if (ret != SPI_OK_INSERT) elog(ERROR, "cannot insert into quota setting table, error code %d", ret);
	}
}

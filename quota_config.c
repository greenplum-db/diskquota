
#include "postgres.h"

#include <sys/stat.h>

#include "access/aomd.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_namespace.h"
#include "catalog/indexing.h"
#include "commands/dbcommands.h"
#include "commands/extension.h"
#include "commands/tablespace.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "storage/proc.h"
#include "utils/snapmgr.h"
#include "utils/builtins.h"
#include "utils/faultinjector.h"
#include "utils/fmgroids.h"
#include "utils/formatting.h"
#include "utils/numeric.h"
#include "libpq-fe.h"
#include "funcapi.h"

#include <cdb/cdbvars.h>
#include <cdb/cdbdisp_query.h>
#include <cdb/cdbdispatchresult.h>

#include "diskquota.h"
#include "quota_config.h"

/*
 * three types values for "quota" column in "quota_config" table:
 * 1) more than 0: valid value
 * 2) 0: meaningless value, rejected by diskquota UDF
 * 3) less than 0: to delete the quota config in the table
 *
 * the values for segratio column are the same as quota column
 *
 * In quota_config table,
 * 1) when quota type is "TABLESPACE_QUOTA",
 *    the quota column value is always INVALID_QUOTA
 * 2) when quota type is "NAMESPACE_TABLESPACE_QUOTA" or "ROLE_TABLESPACE_QUOTA"
 *    and no segratio configed for the tablespace, the segratio value is
 *    INVALID_SEGRATIO.
 * 3) when quota type is "NAMESPACE_QUOTA" or "ROLE_QUOTA", the segratio is
 *    always INVALID_SEGRATIO.
 */
#define INVALID_SEGRATIO 0.0
#define INVALID_QUOTA 0

typedef struct QuotaConfig
{
	Oid       db_oid;
	Oid       owner_oid;
	Oid       namespace_oid;
	Oid       tablespace_oid;
	int64     quota_limit_mb;
	float4    segratio;
	QuotaType quota_type;
} QuotaConfig;

/* JSON parser function */
static char  *construct_quota_config_json(QuotaConfig *config);
static Oid    JSONGetOid(cJSON *head, const char *key);
static int64  JSONGetInt64(cJSON *head, const char *key);
static float4 JSONGetFloat4(cJSON *head, const char *key);
static void   init_cjson_hook();
static void   parse_quota_config(Oid db_oid, QuotaType type, const char *config_str, QuotaConfig *config);
static char  *construct_quota_config_search_condition_json(QuotaConfig *config);

/* quota config function */
static Datum __get_oid_auto_case_convert(Oid (*f)(const char *name, bool missing_ok), const char *name);
static bool  to_delete_quota(QuotaType type, int64 quota_limit_mb, float4 segratio);
static void  set_quota_config_internal(QuotaConfig *config, bool need_del_quota);

PG_FUNCTION_INFO_V1(set_schema_quota_3);

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
	set_quota_config_internal(&config, need_delete_quota);
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

static void
set_quota_config_internal(QuotaConfig *config, bool need_del_quota)
{
	int            ret;
	char          *config_json_str        = construct_quota_config_json(config);
	char          *config_search_json_str = construct_quota_config_search_condition_json(config);
	StringInfoData buffer;
	elog(WARNING, "config_json_str: %s", config_json_str);
	elog(WARNING, "config_search_json_str: %s", config_search_json_str);

	/*
	 * If error happens in set_quota_config_internal, just return error messages to
	 * the client side. So there is no need to catch the error.
	 */
	
	ret = SPI_execute_with_args(
	        "select config::text from diskquota.quota_config_3 where databaseOid = $1 and quotaType = $2 and config @> $3::jsonb", 3,
	        (Oid[]){
	                OIDOID,
	                INT4OID,
	                TEXTOID,
	        },
	        (Datum[]){
	                ObjectIdGetDatum(config->db_oid),
	                Int32GetDatum(config->quota_type),
	                CStringGetDatum(config_search_json_str),
	        },
	        NULL, true, 0);
	// char sql_str[1000];
	// sprintf(sql_str,
	//         "select config from diskquota.quota_config_3 where databaseOid = %d and quotaType = %d and config @>
	//         '%s';", config->db_oid, config->quota_type, config_search_json_str);
	// elog(WARNING, "[select before] %s", sql_str);
	// ret = SPI_execute(sql_str, true, 0);
	elog(WARNING, "[select finished]");
	if (ret != SPI_OK_SELECT)
		elog(ERROR, "cannot select quota setting table: error code %d", ret);
	else
		return;

	if (need_del_quota)
	{
		if (SPI_processed > 0)
		{
			ret = SPI_execute_with_args(
			        "delete from diskquota.quota_config_3 where databaseOid = $1 and quotaType = $2 and config @> $3",
			        3,
			        (Oid[]){
			                OIDOID,
			                INT4OID,
			                JSONBOID,
			        },
			        (Datum[]){
			                ObjectIdGetDatum(config->db_oid),
			                Int32GetDatum(config->quota_type),
			                CStringGetDatum(config_search_json_str),
			        },
			        NULL, false, 0);
			if (ret != SPI_OK_DELETE) elog(ERROR, "cannot delete item from quota setting table, error code %d", ret);
		}
		// else do nothing
	}
	// to upsert quota_config
	else
	{
		if (SPI_processed == 0)
		{
			ret = SPI_execute_with_args("insert into diskquota.quota_config_3 values($1, $2, $3)", 3,
			                            (Oid[]){
			                                    OIDOID,
			                                    INT4OID,
			                                    JSONBOID,
			                            },
			                            (Datum[]){
			                                    ObjectIdGetDatum(config->db_oid),
			                                    Int32GetDatum(config->quota_type),
			                                    CStringGetDatum(config_json_str),
			                            },
			                            NULL, false, 0);
			if (ret != SPI_OK_INSERT) elog(ERROR, "cannot insert into quota setting table, error code %d", ret);
		}
		else
		{
			// TupleDesc tupdesc = SPI_tuptable->tupdesc;
			// HeapTuple tup     = SPI_tuptable->vals[0];
			// Datum     dat;
			// bool      isnull;
			// char     *old_config_json_str = NULL;

			// dat = SPI_getbinval(tup, tupdesc, 1, &isnull);
			// Assert(isnull == false);
			// old_config_json_str = DatumGetCString(dat);

			ret = SPI_execute_with_args(
			        "update diskquota.quota_config_3 set config = $1 where databaseOid = $1 and quotaType = $2 and "
			        "config @> $4",
			        4,
			        (Oid[]){
			                JSONBOID,
			                OIDOID,
			                INT4OID,
			                JSONBOID,
			        },
			        (Datum[]){
			                CStringGetDatum(config_json_str),
			                ObjectIdGetDatum(config->db_oid),
			                Int32GetDatum(config->quota_type),
			                CStringGetDatum(config_search_json_str),
			        },
			        NULL, false, 0);
			if (ret != SPI_OK_UPDATE) elog(ERROR, "cannot update quota setting table, error code %d", ret);
		}
	}
}

static Oid
JSONGetOid(cJSON *head, const char *key)
{
	cJSON *item = cJSON_GetObjectItem(head, key);
	if (cJSON_IsNumber(item)) return (Oid)cJSON_GetNumberValue(item);
	return InvalidOid;
}

static int64
JSONGetInt64(cJSON *head, const char *key)
{
	cJSON *item = cJSON_GetObjectItem(head, key);
	if (cJSON_IsNumber(item)) return (int64)cJSON_GetNumberValue(item);
	return INVALID_QUOTA;
}

static float4
JSONGetFloat4(cJSON *head, const char *key)
{
	cJSON *item = cJSON_GetObjectItem(head, key);
	if (cJSON_IsNumber(item)) return (float4)cJSON_GetNumberValue(item);
	return INVALID_SEGRATIO;
}

static void
init_cjson_hook()
{
	cJSON_Hooks hooks;
	hooks.malloc_fn = palloc;
	hooks.free_fn   = pfree;
	cJSON_InitHooks(&hooks);
}

static void
parse_quota_config(Oid db_oid, QuotaType type, const char *config_str, QuotaConfig *config)
{
	cJSON *head;

	init_cjson_hook();
	head = cJSON_Parse(config_str);

	memset(config, 0, sizeof(QuotaConfig));
	config->quota_type = type;
	config->db_oid     = db_oid;

	switch (type)
	{
		case NAMESPACE_QUOTA:
			config->namespace_oid  = JSONGetOid(head, "namespace_oid");
			config->quota_limit_mb = JSONGetInt64(head, "quota_limit_mb");
			break;
		case ROLE_QUOTA:
			config->owner_oid      = JSONGetOid(head, "owner_oid");
			config->quota_limit_mb = JSONGetInt64(head, "quota_limit_mb");
			break;
		case NAMESPACE_TABLESPACE_QUOTA:
			config->namespace_oid  = JSONGetOid(head, "namespace_oid");
			config->tablespace_oid = JSONGetOid(head, "tablespace_oid");
			config->quota_limit_mb = JSONGetInt64(head, "quota_limit_mb");
			config->segratio       = JSONGetFloat4(head, "segratio");
			break;
		case ROLE_TABLESPACE_QUOTA:
			config->owner_oid      = JSONGetOid(head, "owner_oid");
			config->tablespace_oid = JSONGetOid(head, "tablespace_oid");
			config->quota_limit_mb = JSONGetInt64(head, "quota_limit_mb");
			config->segratio       = JSONGetFloat4(head, "segratio");
			break;
		case TABLESPACE_QUOTA:
			config->tablespace_oid = JSONGetOid(head, "tablespace_oid");
			config->quota_limit_mb = JSONGetInt64(head, "quota_limit_mb");
			config->segratio       = JSONGetFloat4(head, "segratio");
			break;
		default:
			ereport(ERROR,
			        (errcode(ERRCODE_INTERNAL_ERROR), errmsg("[diskquota] quota type is incorrect for: %d", type)));
			break;
	}
}

static char *
construct_quota_config_search_condition_json(QuotaConfig *config)
{
	cJSON *head;

	if (config == NULL) return NULL;
	init_cjson_hook();

	head = cJSON_CreateObject();

	switch (config->quota_type)
	{
		case NAMESPACE_QUOTA:
			cJSON_AddNumberToObject(head, "namespace_oid", config->namespace_oid);
			break;
		case ROLE_QUOTA:
			cJSON_AddNumberToObject(head, "owner_oid", config->owner_oid);
			break;
		case NAMESPACE_TABLESPACE_QUOTA:
			cJSON_AddNumberToObject(head, "namespace_oid", config->namespace_oid);
			cJSON_AddNumberToObject(head, "tablespace_oid", config->tablespace_oid);
			break;
		case ROLE_TABLESPACE_QUOTA:
			cJSON_AddNumberToObject(head, "owner_oid", config->owner_oid);
			cJSON_AddNumberToObject(head, "tablespace_oid", config->tablespace_oid);
			break;
		case TABLESPACE_QUOTA:
			cJSON_AddNumberToObject(head, "tablespace_oid", config->tablespace_oid);
			break;
		default:
			ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
			                errmsg("[diskquota] quota type is incorrect for: %d", config->quota_type)));
			break;
	}
	StringInfoData buffer;
	initStringInfo(&buffer);
	appendStringInfo(&buffer, "'%s'", cJSON_Print(head));
	return buffer.data;
}

static char *
construct_quota_config_json(QuotaConfig *config)
{
	cJSON *head;

	if (config == NULL) return NULL;

	init_cjson_hook();
	head = cJSON_CreateObject();

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
			// cJSON_AddNumberToObject(head, "segratio", config->segratio);
			break;
		case ROLE_TABLESPACE_QUOTA:
			cJSON_AddNumberToObject(head, "owner_oid", config->owner_oid);
			cJSON_AddNumberToObject(head, "tablespace_oid", config->tablespace_oid);
			cJSON_AddNumberToObject(head, "quota_limit_mb", config->quota_limit_mb);
			// cJSON_AddNumberToObject(head, "segratio", config->segratio);
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
	StringInfoData buffer;
	initStringInfo(&buffer);
	appendStringInfo(&buffer, "'%s'", cJSON_Print(head));
	return buffer.data;
}
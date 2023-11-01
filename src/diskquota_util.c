/*-------------------------------------------------------------------------
 *
 * diskquota_util.c
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/diskquota_util.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/acl.h"
#include "utils/numeric.h"
#include "utils/inval.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "executor/spi.h"
#include "commands/tablespace.h"

#include "quota_config.h"
#include "diskquota_util.h"

Datum
get_oid_auto_case_convert(Oid (*f)(const char *name, bool missing_ok), const char *name)
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

/*
 * Convert a human-readable size to a size in MB.
 */
int64
get_size_in_mb(char *str)
{
	char   *strptr, *endptr;
	char    saved_char;
	Numeric num;
	int64   result;
	bool    have_digits = false;

	str = str_tolower(str, strlen(str), DEFAULT_COLLATION_OID);

	/* Skip leading whitespace */
	strptr = str;
	while (isspace((unsigned char)*strptr)) strptr++;

	/* Check that we have a valid number and determine where it ends */
	endptr = strptr;

	/* Part (1): sign */
	if (*endptr == '-' || *endptr == '+') endptr++;

	/* Part (2): main digit string */
	if (isdigit((unsigned char)*endptr))
	{
		have_digits = true;
		do endptr++;
		while (isdigit((unsigned char)*endptr));
	}

	/* Part (3): optional decimal point and fractional digits */
	if (*endptr == '.')
	{
		endptr++;
		if (isdigit((unsigned char)*endptr))
		{
			have_digits = true;
			do endptr++;
			while (isdigit((unsigned char)*endptr));
		}
	}

	/* Complain if we don't have a valid number at this point */
	if (!have_digits) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid size: \"%s\"", str)));

	/* Part (4): optional exponent */
	if (*endptr == 'e' || *endptr == 'E')
	{
		long  exponent;
		char *cp;

		/*
		 * Note we might one day support EB units, so if what follows 'E'
		 * isn't a number, just treat it all as a unit to be parsed.
		 */
		exponent = strtol(endptr + 1, &cp, 10);
		(void)exponent; /* Silence -Wunused-result warnings */
		if (cp > endptr + 1) endptr = cp;
	}

	/*
	 * Parse the number, saving the next character, which may be the first
	 * character of the unit string.
	 */
	saved_char = *endptr;
	*endptr    = '\0';

	num = DatumGetNumeric(
	        DirectFunctionCall3(numeric_in, CStringGetDatum(strptr), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)));

	*endptr = saved_char;

	/* Skip whitespace between number and unit */
	strptr = endptr;
	while (isspace((unsigned char)*strptr)) strptr++;

	/* Handle possible unit */
	if (*strptr != '\0')
	{
		int64 multiplier = 0;

		/* Trim any trailing whitespace */
		endptr = str + strlen(str) - 1;

		while (isspace((unsigned char)*endptr)) endptr--;

		endptr++;
		*endptr = '\0';

		/* Parse the unit case-insensitively */
		if (pg_strcasecmp(strptr, "mb") == 0)
			multiplier = ((int64)1);

		else if (pg_strcasecmp(strptr, "gb") == 0)
			multiplier = ((int64)1024);

		else if (pg_strcasecmp(strptr, "tb") == 0)
			multiplier = ((int64)1024) * 1024;
		else if (pg_strcasecmp(strptr, "pb") == 0)
			multiplier = ((int64)1024) * 1024 * 1024;
		else
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid size: \"%s\"", str),
			                errdetail("Invalid size unit: \"%s\".", strptr),
			                errhint("Valid units are \"MB\", \"GB\", \"TB\", and \"PB\".")));

		if (multiplier > 1)
		{
			Numeric mul_num;

			mul_num = DatumGetNumeric(DirectFunctionCall1(int8_numeric, Int64GetDatum(multiplier)));

			num = DatumGetNumeric(DirectFunctionCall2(numeric_mul, NumericGetDatum(mul_num), NumericGetDatum(num)));
		}
	}

	result = DatumGetInt64(DirectFunctionCall1(numeric_int8, NumericGetDatum(num)));

	return result;
}

void
check_superuser(void)
{
	if (!superuser())
	{
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to set disk quota limit")));
	}
}

char *
GetNamespaceName(Oid spcid, bool skip_name)
{
	if (skip_name)
	{
		NameData spcstr;
		pg_ltoa(spcid, spcstr.data);
		return pstrdup(spcstr.data);
	}
	return get_namespace_name(spcid);
}

char *
GetTablespaceName(Oid spcid, bool skip_name)
{
	if (skip_name)
	{
		NameData spcstr;
		pg_ltoa(spcid, spcstr.data);
		return pstrdup(spcstr.data);
	}
	return get_tablespace_name(spcid);
}

char *
GetUserName(Oid relowner, bool skip_name)
{
	if (skip_name)
	{
		NameData namestr;
		pg_ltoa(relowner, namestr.data);
		return pstrdup(namestr.data);
	}
#if GP_VERSION_NUM < 70000
	return GetUserNameFromId(relowner);
#else
	return GetUserNameFromId(relowner, false);
#endif /* GP_VERSION_NUM */
}

/*
 * Given table oid, search for namespace and owner.
 */
bool
get_rel_owner_schema_tablespace(Oid relid, Oid *ownerOid, Oid *nsOid, Oid *tablespaceoid)
{
	HeapTuple tp;

	/*
	 * Since we don't take any lock on relation, check for cache
	 * invalidation messages manually to minimize risk of cache
	 * inconsistency.
	 */
	AcceptInvalidationMessages();
	tp         = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	bool found = HeapTupleIsValid(tp);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class)GETSTRUCT(tp);

		*ownerOid      = reltup->relowner;
		*nsOid         = reltup->relnamespace;
		*tablespaceoid = reltup->reltablespace;

		if (!OidIsValid(*tablespaceoid))
		{
			*tablespaceoid = MyDatabaseTableSpace;
		}

		ReleaseSysCache(tp);
	}
	return found;
}

/*
 * Given table oid, search for namespace and name.
 * Memory relname points to should be pre-allocated at least NAMEDATALEN bytes.
 */
bool
get_rel_name_namespace(Oid relid, Oid *nsOid, char *relname)
{
	HeapTuple tp;

	/*
	 * Since we don't take any lock on relation, check for cache
	 * invalidation messages manually to minimize risk of cache
	 * inconsistency.
	 */
	AcceptInvalidationMessages();
	tp         = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	bool found = HeapTupleIsValid(tp);
	if (found)
	{
		Form_pg_class reltup = (Form_pg_class)GETSTRUCT(tp);

		*nsOid = reltup->relnamespace;
		memcpy(relname, reltup->relname.data, NAMEDATALEN);

		ReleaseSysCache(tp);
	}
	return found;
}

#include "postgres.h"

#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/acl.h"
#include "utils/numeric.h"
#include "executor/spi.h"

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

static float4
get_per_segment_ratio(Oid spcoid)
{
	int    ret;
	float4 segratio = INVALID_SEGRATIO;

	if (!OidIsValid(spcoid)) return segratio;

	/*
	 * using row share lock to lock TABLESPACE_QUTAO
	 * row to avoid concurrently updating the segratio
	 */
	ret = SPI_execute_with_args(
	        "select segratio from diskquota.quota_config where targetoid = $1 and quotatype = $2 for share", 2,
	        (Oid[]){
	                OIDOID,
	                INT4OID,
	        },
	        (Datum[]){
	                ObjectIdGetDatum(spcoid),
	                Int32GetDatum(TABLESPACE_QUOTA),
	        },
	        NULL, false, 0);
	if (ret != SPI_OK_SELECT)
	{
		elog(ERROR, "cannot get per segment ratio for the tablepace: error code %d", ret);
	}

	if (SPI_processed == 1)
	{
		TupleDesc tupdesc = SPI_tuptable->tupdesc;
		HeapTuple tup     = SPI_tuptable->vals[0];
		Datum     dat;
		bool      isnull;

		dat = SPI_getbinval(tup, tupdesc, 1, &isnull);
		if (!isnull)
		{
			segratio = DatumGetFloat4(dat);
		}
	}
	return segratio;
}
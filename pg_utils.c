/* -------------------------------------------------------------------------
 *
 * pg_utils.c
 *
 * This code is utils for detecting active table for databases
 *
 * Copyright (C) 2013, PostgreSQL Global Development Group
 *
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "fmgr.h"
#include "utils/fmgrprotos.h"

#include "pg_utils.h"

#include <sys/stat.h>

int64 diskquota_get_table_size_by_oid(Oid oid);
int64 diskquota_get_table_size_by_relfilenode(RelFileNode *rfh);
/*
 * calculate size of (one fork of) a table in transaction
 * This function is following calculate_relation_size()
 */
int64
diskquota_get_table_size_by_relfilenode(RelFileNode *rfn)
{
    int64       totalsize = 0;
    ForkNumber  forkNum;
    int64       size = 0;
    char       *relationpath;
    char        pathname[MAXPGPATH];
    unsigned int segcount = 0;

    for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
    {
        relationpath = relpathperm(*rfn, forkNum);
        size = 0;

        for (segcount = 0;; segcount++)
        {
            struct stat fst;

            CHECK_FOR_INTERRUPTS();

            if (segcount == 0)
                snprintf(pathname, MAXPGPATH, "%s",
                         relationpath);
            else
                snprintf(pathname, MAXPGPATH, "%s.%u",
                         relationpath, segcount);

            if (stat(pathname, &fst) < 0)
            {
                if (errno == ENOENT)
                    break;
                else
                    ereport(ERROR,
                            (errcode_for_file_access(),
                                errmsg("could not stat file \"%s\": %m", pathname)));
            }
            size += fst.st_size;
        }

        totalsize += size;
    }

    return totalsize;
}

/*
 * Function to calculate the total relation size
 */
int64
diskquota_get_table_size_by_oid(Oid oid)
{
    FunctionCallInfoData fcinfo;
    Datum       result;

    InitFunctionCallInfoData(fcinfo, NULL, 1, InvalidOid, NULL, NULL);

    fcinfo.arg[0] = ObjectIdGetDatum(oid);
    fcinfo.argnull[0] = false;

    result = pg_total_relation_size(&fcinfo);

    /* Check for null result, since caller is clearly not expecting one */
    if (fcinfo.isnull)
        return 0;

    return DatumGetInt64(result);
}

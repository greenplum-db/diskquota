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

#include "gp_utils.h"

#include <sys/stat.h>

Size diskquota_get_relation_size_by_relfilenode(RelFileNode *rfn);

/*
 * calculate size of (one fork of) a table in transaction
 * This function is following calculate_relation_size()
 */
Size
diskquota_get_relation_size_by_relfilenode(RelFileNode *rfn)
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
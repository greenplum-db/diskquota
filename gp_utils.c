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

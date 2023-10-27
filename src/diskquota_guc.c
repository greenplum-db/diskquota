/*-------------------------------------------------------------------------
 *
 * diskquota_guc.c
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/diskquota_guc.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"
#include "postgres.h"

#include "diskquota_guc.h"
#include "diskquota.h"
#include "table_size.h"

#include "utils/guc.h"
#include "limits.h"

/* GUC variables */
int  diskquota_naptime                 = 0;
int  diskquota_max_active_tables       = 0;
int  diskquota_worker_timeout          = 60; /* default timeout is 60 seconds */
bool diskquota_hardlimit               = false;
int  diskquota_max_workers             = 10;
int  diskquota_max_table_segments      = 0;
int  diskquota_max_monitored_databases = 0;
int  diskquota_max_quota_probes        = 0;

/*
 * Define GUC variables used by diskquota
 */
void
define_guc_variables(void)
{
#if DISKQUOTA_DEBUG
	const int min_naptime = 0;
#else
	const int min_naptime = 1;
#endif

	DefineCustomIntVariable("diskquota.naptime", "Duration between each check (in seconds).", NULL, &diskquota_naptime,
	                        2, min_naptime, INT_MAX, PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("diskquota.max_active_tables", "Max number of active tables monitored by disk-quota.", NULL,
	                        &diskquota_max_active_tables, 300 * 1024, 1, INT_MAX, PGC_POSTMASTER, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("diskquota.worker_timeout", "Duration between each check (in seconds).", NULL,
	                        &diskquota_worker_timeout, 60, 1, INT_MAX, PGC_SIGHUP, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("diskquota.hard_limit", "Set this to 'on' to enable disk-quota hardlimit.", NULL,
	                         &diskquota_hardlimit, false, PGC_SIGHUP, 0, NULL, NULL, NULL);
	DefineCustomIntVariable(
	        "diskquota.max_workers",
	        "Max number of backgroud workers to run diskquota extension, should be less than max_worker_processes.",
	        NULL, &diskquota_max_workers, 10, 1, 20, PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("diskquota.max_table_segments", "Max number of tables segments on the cluster.", NULL,
	                        &diskquota_max_table_segments, 10 * 1024 * 1024, INIT_NUM_TABLE_SIZE_ENTRIES * 1024,
	                        INT_MAX, PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("diskquota.max_monitored_databases", "Max number of database on the cluster.", NULL,
	                        &diskquota_max_monitored_databases, 50, 1, 1024, PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("diskquota.max_quota_probes", "Max number of quotas on the cluster.", NULL,
	                        &diskquota_max_quota_probes, 1024 * 1024, 1024 * INIT_QUOTA_MAP_ENTRIES, INT_MAX,
	                        PGC_POSTMASTER, 0, NULL, NULL, NULL);
}

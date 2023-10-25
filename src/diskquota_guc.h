/*-------------------------------------------------------------------------
 *
 * diskquota_guc.h
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/diskquota_guc.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef DISKQUOTA_GUC_H
#define DISKQUOTA_GUC_H

/* GUC variables */
extern int  diskquota_naptime;
extern int  diskquota_max_active_tables;
extern int  diskquota_worker_timeout;
extern bool diskquota_hardlimit;
extern int  diskquota_max_workers;
extern int  diskquota_max_table_segments;
extern int  diskquota_max_monitored_databases;
extern int  diskquota_max_quota_probes;

extern void define_guc_variables(void);

#endif

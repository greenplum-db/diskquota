/*-------------------------------------------------------------------------
 *
 * diskquota_launcher.h
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/diskquota_launcher.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef DISKQUOTA_LAUNCHER_H
#define DISKQUOTA_LAUNCHER_H

extern void init_launcher_shmem(void);
extern Size diskquota_launcher_shmem_size(void);

extern bool is_dynamic_mode(void);
extern void FreeWorker(DiskQuotaWorkerEntry *worker);

#endif

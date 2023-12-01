/*-------------------------------------------------------------------------
 *
 * diskquota_center_worker.h
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/diskquota_center_worker.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef DISKQUOTA_CENTER_WORKER_H
#define DISKQUOTA_CENTER_WORKER_H

#define DISKQUOTA_CENTER_WORKER_NAME "diskquota_center_worker"
#define DISKQUOTA_CENTER_WORKER_MESSAGE_LOOPER_NAME "diskquota_center_worker_message_looper"

extern Size diskquota_center_worker_shmem_size(void);

#endif

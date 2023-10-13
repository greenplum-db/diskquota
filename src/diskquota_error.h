/* -------------------------------------------------------------------------
 *
 * diskquota_error.h
 *
 * Copyright (c) 2023-Present VMware, Inc. or its affiliates
 *
 * IDENTIFICATION
 *		diskquota/diskquota_error.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef DISKQUOTA_ERROR_H
#define DISKQUOTA_ERROR_H

#include "c.h"

#define BGWORKER_ERROR_MAP_SIZE 1024

typedef enum
{
	UPDATE_TABLE_SIZE_ERROR
} DiskQuotaBGworkerError;

typedef struct BGworkerErrorEntry
{
	DiskQuotaBGworkerError error;
} BGworkerErrorEntry;

extern void                    init_bgworker_error_map(uint32 id);
extern int                     diskquota_status_get_error_num(void);
extern void                    diskquota_status_push_error(DiskQuotaBGworkerError error);
extern const char             *diskquota_status_error_to_string(DiskQuotaBGworkerError error);
extern const char             *diskquota_status_error_hint(DiskQuotaBGworkerError error);
extern DiskQuotaBGworkerError *diskquota_status_error_map_to_array(void);

#endif

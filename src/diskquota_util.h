/*-------------------------------------------------------------------------
 *
 * diskquota_util.h
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/diskquota_util.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef DISKQUOTA_UTIL_H
#define DISKQUOTA_UTIL_H

extern Datum get_oid_auto_case_convert(Oid (*f)(const char *name, bool missing_ok), const char *name);
extern int64 get_size_in_mb(char *str);

#endif

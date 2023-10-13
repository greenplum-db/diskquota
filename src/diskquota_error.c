/* -------------------------------------------------------------------------
 *
 * diskquota_error.c
 *
 * Copyright (c) 2023-Present VMware, Inc. or its affiliates
 *
 * IDENTIFICATION
 *		diskquota/diskquota_error.c
 *
 * -------------------------------------------------------------------------
 */

#include "diskquota.h"
#include "diskquota_error.h"

/* error map in bgworker */
HTAB *diskquota_bgworker_error_map;

void
init_bgworker_error_map(uint32 id)
{
	StringInfoData str;
	HASHCTL        hash_ctl;

	initStringInfo(&str);
	appendStringInfo(&str, "BGworkerErrorMap_%u", id);
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize             = sizeof(BGworkerErrorEntry);
	hash_ctl.entrysize           = sizeof(BGworkerErrorEntry);
	diskquota_bgworker_error_map = DiskquotaShmemInitHash(str.data, BGWORKER_ERROR_MAP_SIZE, BGWORKER_ERROR_MAP_SIZE,
	                                                      &hash_ctl, HASH_ELEM, DISKQUOTA_TAG_HASH);
}

void
diskquota_status_push_error(DiskQuotaBGworkerError error)
{
	hash_search(diskquota_bgworker_error_map, &error, HASH_ENTER, NULL);
}

int
diskquota_status_get_error_num(void)
{
	return hash_get_num_entries(diskquota_bgworker_error_map);
}

DiskQuotaBGworkerError *
diskquota_status_error_map_to_array(void)
{
	DiskQuotaBGworkerError *array;
	HASH_SEQ_STATUS         iter;
	BGworkerErrorEntry     *entry;
	int                     idx = 0;

	array = palloc0(diskquota_status_get_error_num() * sizeof(DiskQuotaBGworkerError));
	hash_seq_init(&iter, diskquota_bgworker_error_map);
	while ((entry = hash_seq_search(&iter)) != NULL) memcpy(&array[idx++], entry, sizeof(BGworkerErrorEntry));

	return array;
}

const char *
diskquota_status_error_to_string(DiskQuotaBGworkerError error)
{
	switch (error)
	{
		case UPDATE_TABLE_SIZE_ERROR:
			return "UPDATE_TABLE_SIZE_ERROR";
		default:
			break;
	}
	return "";
}

const char *
diskquota_status_error_hint(DiskQuotaBGworkerError error)
{
	switch (error)
	{
		case UPDATE_TABLE_SIZE_ERROR:
			return "Table size update failed, please run 'select diskquota.init_table_size_table()' to initialize "
			       "diskquota.";
		default:
			break;
	}
	return "";
}

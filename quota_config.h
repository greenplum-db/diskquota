/* -------------------------------------------------------------------------
 *
 * quota_config.h
 *
 * Copyright (c) 2018-2020 Pivotal Software, Inc.
 * Copyright (c) 2020-Present VMware, Inc. or its affiliates
 *
 * IDENTIFICATION
 *		diskquota/quota_config.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef QUOTA_CONFIG_H
#define QUOTA_CONFIG_H

/*
 * three types values for "quota" column in "quota_config" table:
 * 1) more than 0: valid value
 * 2) 0: meaningless value, rejected by diskquota UDF
 * 3) less than 0: to delete the quota config in the table
 *
 * the values for segratio column are the same as quota column
 *
 * In quota_config table,
 * 1) when quota type is "TABLESPACE_QUOTA",
 *    the quota column value is always INVALID_QUOTA
 * 2) when quota type is "NAMESPACE_TABLESPACE_QUOTA" or "ROLE_TABLESPACE_QUOTA"
 *    and no segratio configed for the tablespace, the segratio value is
 *    INVALID_SEGRATIO.
 * 3) when quota type is "NAMESPACE_QUOTA" or "ROLE_QUOTA", the segratio is
 *    always INVALID_SEGRATIO.
 */
#define INVALID_SEGRATIO 0.0
#define INVALID_QUOTA 0

#define MAX_QUOTA_NUM 1024L

typedef struct QuotaConfig
{
	QuotaType quota_type;
	Oid       db_oid;
	Oid       owner_oid;
	Oid       namespace_oid;
	Oid       tablespace_oid;
	int64     quota_limit_mb;
	float4    segratio;
} QuotaConfig;

typedef struct QuotaConfigKey
{
	QuotaType quota_type;
	Oid       db_oid;
	Oid       owner_oid;
	Oid       namespace_oid;
	Oid       tablespace_oid;
} QuotaConfigKey;

#endif

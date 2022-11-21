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
 * three types values for quota_limit_mb/segratio in "quota_config" table:
 * 1) more than 0: valid value
 * 2) 0: meaningless value, rejected by diskquota UDF
 * 3) less than 0: to delete the quota config
 *
 * In quota_config table,
 * 1) when quota type is TABLESPACE_QUOTA, quota_limit_mb value is INVALID_QUOTA
 * 2) when quota type is NAMESPACE_TABLESPACE_QUOTA, ROLE_TABLESPACE_QUOTA,
 *    NAMESPACE_QUOTA or ROLE_QUOTA, the segratio value is INVALID_SEGRATIO.
 * 3) when quota type is NAMESPACE_TABLESPACE_QUOTA or ROLE_TABLESPACE_QUOTA, the
 *    segratio value is read from relevant TABLESPACE_QUOTA. If the TABLESPACE_QUOTA
 *    is not set, the segratio value is INVALID_SEGRATIO.
 */
#define INVALID_SEGRATIO 0.0
#define INVALID_QUOTA 0

#define MAX_QUOTA_NUM 1024L
#define MAX_QUOTA_KEY_NUM 8

typedef enum
{
	NAMESPACE_QUOTA = 0,
	ROLE_QUOTA,
	NAMESPACE_TABLESPACE_QUOTA,
	ROLE_TABLESPACE_QUOTA,
	/*
	 * TABLESPACE_QUOTA
	 * when set_per_segment_quota("xx",1.0) is called
	 * to set per segment quota to '1.0', the config
	 * will be:
	 * quota_type = TABLESPACE_QUOTA
	 * segratio = 1.0
	 */
	TABLESPACE_QUOTA,

	NUM_QUOTA_TYPES,
} QuotaType;

typedef struct QuotaConfig
{
	QuotaType quota_type;
	Oid       keys[MAX_QUOTA_KEY_NUM];
	int64     quota_limit_mb;
	float4    segratio;
} QuotaConfig;

typedef struct QuotaConfigKey
{
	QuotaType quota_type;
	Oid       keys[MAX_QUOTA_KEY_NUM];
} QuotaConfigKey;

#endif

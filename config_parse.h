/* -------------------------------------------------------------------------
 *
 * config_parse.h
 *
 * Copyright (c) 2018-2020 Pivotal Software, Inc.
 * Copyright (c) 2020-Present VMware, Inc. or its affiliates
 *
 * IDENTIFICATION
 *		diskquota/config_parse.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef CONFIG_PARSE_H
#define CONFIG_PARSE_H

#include "cjson/cJSON.h"

/* JSON parser function */
void  JSON_parse_quota_config(Oid db_oid, const char *config_str, HTAB *quota_config_map);
char *JSON_construct_quota_config(Oid db_oid, HTAB *quota_config_map);

#endif

/*-------------------------------------------------------------------------
 *
 * config_parse.h
 *
 * Portions Copyright (c) 2023-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	   src/config_parse.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef CONFIG_PARSE_H
#define CONFIG_PARSE_H

#include "cjson/cJSON.h"

typedef void *(*malloc_fn)(size_t sz);
typedef void (*free_fn)(void *ptr);

/* JSON parser function */
extern void      init_cjson_hook(malloc_fn mfn, free_fn ffn);
extern char     *JSON_get_version(cJSON *head);
extern cJSON    *JSON_get_quota_list(cJSON *head);
extern QuotaType JSON_get_quota_type(cJSON *head, const char *key);
extern cJSON    *do_construct_quota_config(QuotaConfig *config);
extern int       do_parse_quota_config(cJSON *head, QuotaConfig *config);
#endif

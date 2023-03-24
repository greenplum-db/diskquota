#include "gp_mock.h"
#include "quota_config.h"
#include "config_parse.h"

char *quota_type     = "\"quota_type\"";
char *namespace_oid  = "\"namespace_oid\"";
char *quota_limit_mb = "\"quota_limit_mb\"";

static void
test_parse(void **state)
{
	char expect_str[100];
	sprintf(expect_str, "{%s: %d, %s: %d, %s: %d}", quota_type, 1, namespace_oid, 2, quota_limit_mb, 3);
	printf("%s\n", expect_str);
}

static void
test_JSON_parse_quota_config_same_version(void **state)
{
	HASHCTL ctl;
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize            = sizeof(QuotaConfigKey);
	ctl.entrysize          = sizeof(QuotaConfig);
	HTAB *quota_config_map = hash_create("quota_config_map", 1024, &ctl, 0);

	char *json_str = "{\"version\":\"diskquota-3.0\",\"quota_list\":[]}";

	cJSON *expected_json     = cJSON_Parse(json_str);
	char  *expected_json_str = cJSON_Print(expected_json);

	JSON_parse_quota_config(json_str, quota_config_map);
	char  *parsed_json_str = JSON_construct_quota_config(quota_config_map);
	cJSON *parsed_json     = cJSON_Parse(parsed_json_str);

	printf("%s\n", json_str);
	printf("%s\n", expected_json_str);
	printf("%s\n", parsed_json_str);

	assert_true(cJSON_Compare(expected_json, parsed_json, true));
	assert_memory_equal(parsed_json_str, expected_json_str, strlen(expected_json_str));
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
	        cmocka_unit_test(test_parse),
	        cmocka_unit_test(test_pull_quota_config),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}

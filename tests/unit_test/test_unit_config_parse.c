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

static int
read_input_string(char *file_name, char **str)
{
	FILE *fp;
	int   file_size;
	int   len;

	fp = fopen(file_name, "r");
	printf("%p %s\n", fp, file_name);
	assert_non_null(fp);

	fseek(fp, 0, SEEK_END);
	file_size = ftell(fp);

	*str = palloc(file_size + 5);
	memset(*str, 0, file_size + 5);

	fseek(fp, 0, SEEK_SET);
	len = fread(*str, sizeof(char), file_size, fp);

	fclose(fp);
	return len;
}

static void
test_JSON_parse_quota_config_same_version(void **state)
{
	HASHCTL ctl;
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize            = sizeof(QuotaConfigKey);
	ctl.entrysize          = sizeof(QuotaConfig);
	HTAB *quota_config_map = hash_create("quota_config_map", 1024, &ctl, 0);

	char *json_str;
	char  file_name[100];
	sprintf(file_name, "%s/JSON_parse_quota_config_same_version.json", UT_INPUT_FILE_DIR);
	int len = read_input_string(file_name, &json_str);

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

	pfree(json_str);
	pfree(expected_json_str);
	pfree(parsed_json_str);
	cJSON_Delete(expected_json);
	cJSON_Delete(parsed_json);
}

int
main(int argc, char *argv[])
{
	const struct CMUnitTest tests[] = {
	        cmocka_unit_test(test_parse),
	        cmocka_unit_test(test_JSON_parse_quota_config_same_version),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}

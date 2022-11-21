#include "c.h"

#include "quota_config.h"
#include "config_parse.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <cmocka.h>

#ifdef sprintf
#undef sprintf
#endif
#ifdef printf
#undef printf
#endif

char *quota_type     = "\"quota_type\"";
char *db_oid         = "\"db_oid\"";
char *namespace_oid  = "\"namespace_oid\"";
char *quota_limit_mb = "\"quota_limit_mb\"";

static void
test_parse(void **state)
{
	char        expect_str[100];
	QuotaConfig config;

	memset(&config, 0, sizeof(config));
	config.quota_type     = NAMESPACE_QUOTA;
	config.keys[0]        = 123;
	config.keys[1]        = 456;
	config.quota_limit_mb = 100;

	sprintf(expect_str, "{%s: %d, %s: %d, %s: %d, %s: %ld}", quota_type, NAMESPACE_QUOTA, db_oid, config.keys[0],
	        namespace_oid, config.keys[1], quota_limit_mb, config.quota_limit_mb);

	cJSON *ret         = do_construct_quota_config(&config);
	cJSON *expect_json = cJSON_Parse(expect_str);

	char *a = cJSON_Print(ret), *b = cJSON_Print(expect_json);
	assert_memory_equal(a, b, strlen(a));
	printf("%s\n%s\n", a, b);
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
	        cmocka_unit_test(test_parse),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
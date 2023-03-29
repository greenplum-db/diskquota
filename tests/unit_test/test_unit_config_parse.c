#include "c.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <cmocka.h>

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
test_parse_fail(void **state)
{
	char expect_str[100];
	sprintf(expect_str, "{%s: %d, %s: %d, %s: %d}", quota_type, 1, namespace_oid, 2, quota_limit_mb, 3);
	printf("%s\n", expect_str);
	assert_true(false);
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
	        cmocka_unit_test(test_parse),
	        // cmocka_unit_test(test_parse_fail),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
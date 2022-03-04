# add new version to upgrade or downgrade test

- add a new `schedule` file like `schedule_10.1--10.0`.
- write those new test:

```
test: 10.1_install
test: 10.1_set_quota
test: 10.1_catalog
test: 10.0_migrate_to_version_10.0
test: 10.0_catalog
test: 10.1_test_in_10.0_quota_create_in_10.1
test: 10.1_cleanup_quota
```

the file name means this is a downgrade test from 10.1 to 10.0.

for upgrade test, just reverse the schedule file.

---

`10.1_test_in_10.0_quota_create_in_10.1` means:

- the file is for version 10.1
- this is a test file
- the test occur in 10.0, use 10.0 binary and 10.0 SQL
- the item to test is created in 10.1
----

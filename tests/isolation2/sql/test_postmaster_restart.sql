1: CREATE SCHEMA postmaster_restart_s;
1: SET search_path TO postmaster_restart_s;

1: SELECT diskquota.enable_hardlimit();
1: SELECT diskquota.set_schema_quota('postmaster_restart_s', '1 MB');
1: SELECT diskquota.wait_for_worker_new_epoch();

-- expect fail
1: CREATE TABLE t1 AS SELECT generate_series(1,1000000);
1q:

-- launcher should exist
!\retcode pgrep -a -f "postgres.*launcher";
-- bgworker should exist
!\retcode pgrep -a -f "postgres.*diskquota.*isolation2test";

-- stop postmaster
!\retcode pg_ctl -D $MASTER_DATA_DIRECTORY -w stop;

-- launcher should be terminated
!\retcode pgrep -a -f "postgres.*launcher";
-- bgworker should be terminated
!\retcode pgrep -a -f "postgres.*diskquota.*isolation2test";

-- start postmaster
!\retcode pg_ctl -D $MASTER_DATA_DIRECTORY -w -o "-E" start;

-- launcher should be restarted
!\retcode pgrep -a -f "postgres.*launcher";
-- bgworker should be restarted
!\retcode pgrep -a -f "postgres.*diskquota.*isolation2test";

1: SET search_path TO postmaster_restart_s;
-- hardlimit needs to be re-enabled
1: SELECT diskquota.enable_hardlimit();
1: SELECT diskquota.wait_for_worker_new_epoch();
-- expect fail
1: CREATE TABLE t2 AS SELECT generate_series(1,1000000);
-- enlarge the quota limits
1: SELECT diskquota.set_schema_quota('postmaster_restart_s', '100 MB');
1: SELECT diskquota.wait_for_worker_new_epoch();
-- expect succeed
1: CREATE TABLE t3 AS SELECT generate_series(1,1000000);

1: DROP SCHEMA postmaster_restart_s CASCADE;
1q:

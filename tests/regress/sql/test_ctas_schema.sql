-- Test that diskquota is able to cancel a running CTAS query by the schema quota.
SELECT diskquota.enable_hardlimit();
CREATE SCHEMA hardlimit_s;
SELECT diskquota.set_schema_quota('hardlimit_s', '1 MB');
SET search_path TO hardlimit_s;

-- heap table
CREATE TABLE t1 AS SELECT generate_series(1, 100000000);
SELECT diskquota.wait_for_worker_new_epoch();

-- toast table
CREATE TABLE toast_table
  AS SELECT ARRAY(SELECT generate_series(1,10000)) FROM generate_series(1, 100000);
SELECT diskquota.wait_for_worker_new_epoch();

-- ao table
CREATE TABLE ao_table WITH (appendonly=true) AS SELECT generate_series(1, 100000000);
SELECT diskquota.wait_for_worker_new_epoch();

-- aocs table
CREATE TABLE aocs_table WITH (appendonly=true, orientation=column)
  AS SELECT i, ARRAY(SELECT generate_series(1,10000)) FROM generate_series(1, 100000) AS i;
SELECT diskquota.wait_for_worker_new_epoch();

-- disable hardlimit and do some clean-ups.
SELECT diskquota.disable_hardlimit();
DROP TABLE IF EXISTS t1;
DROP TABLE IF EXISTS toast_table;
DROP TABLE IF EXISTS ao_table;
DROP TABLE IF EXISTS aocs_table;
RESET search_path;
DROP SCHEMA hardlimit_s;

--start_ignore
CREATE DATABASE test_db_cache;
--end_ignore

\c test_db_cache
CREATE EXTENSION diskquota;
CREATE EXTENSION diskquota_test;

-- Wait until the db cache gets updated 
SELECT diskquota.wait_for_worker_new_epoch();

CREATE TABLE t(i) AS SELECT generate_series(1, 100000)
DISTRIBUTED BY (i);

SELECT diskquota.wait_for_worker_new_epoch();

SELECT tableid::regclass, size, segid
FROM diskquota.table_size
WHERE tableid = 't'::regclass
ORDER BY segid;

DROP TABLE t;
DROP EXTENSION diskquota;

\c contrib_regression
DROP DATABASE test_db_cache;

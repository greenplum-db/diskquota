--
-- This file contains tests for limiting reject map
--

CREATE DATABASE test_reject_map_limit_01;

\c test_reject_map_limit_01
CREATE EXTENSION diskquota;
SELECT diskquota.wait_for_worker_new_epoch();
-- we only read the current log file
CREATE EXTERNAL WEB TABLE master_log(line text)
    EXECUTE 'cat $GP_SEG_DATADIR/pg_log/$(ls -Art $GP_SEG_DATADIR/pg_log | tail -n 1)'
    ON MASTER FORMAT 'TEXT' (DELIMITER 'OFF');

CREATE SCHEMA s1;
CREATE SCHEMA s2;
CREATE SCHEMA s3;
CREATE SCHEMA s4;
CREATE SCHEMA s5;

SELECT diskquota.set_schema_quota('s1', '1 MB');
SELECT diskquota.set_schema_quota('s2', '1 MB');
SELECT diskquota.set_schema_quota('s3', '1 MB');
SELECT diskquota.set_schema_quota('s4', '1 MB');
SELECT diskquota.set_schema_quota('s5', '1 MB');
SELECT diskquota.wait_for_worker_new_epoch();

CREATE TABLE s1.a(i int) DISTRIBUTED BY (i);
CREATE TABLE s2.a(i int) DISTRIBUTED BY (i);
CREATE TABLE s3.a(i int) DISTRIBUTED BY (i);
CREATE TABLE s4.a(i int) DISTRIBUTED BY (i);
CREATE TABLE s5.a(i int) DISTRIBUTED BY (i);


\! gpconfig -c diskquota.max_active_tables -v 4 > /dev/null
\! gpstop -arf > /dev/null

\c test_reject_map_limit_01

INSERT INTO s1.a SELECT generate_series(1,100000);
INSERT INTO s2.a SELECT generate_series(1,100000);
INSERT INTO s3.a SELECT generate_series(1,100000);
INSERT INTO s4.a SELECT generate_series(1,100000);
INSERT INTO s5.a SELECT generate_series(1,100000);

SELECT diskquota.wait_for_worker_new_epoch();

SELECT count(*) FROM master_log WHERE line LIKE '%the number of local quota reject map entries reached the limit%' AND line NOT LIKE '%LOG%';

DROP EXTENSION diskquota;

\c contrib_regression
DROP DATABASE test_reject_map_limit_01;

\! gpconfig -r diskquota.max_active_tables > /dev/null
\! gpstop -arf > /dev/null

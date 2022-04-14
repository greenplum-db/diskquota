-- table in 'diskquota not enabled database' should not be activetable
\! gpconfig -c diskquota.max_active_tables -v 2 > /dev/null
\! gpstop -arf > /dev/null

\c

CREATE DATABASE test_tablenum_limit_01;
CREATE DATABASE test_tablenum_limit_02;

\c test_tablenum_limit_01

CREATE TABLE a01(i int) DISTRIBUTED BY (i);
CREATE TABLE a02(i int) DISTRIBUTED BY (i);
CREATE TABLE a03(i int) DISTRIBUTED BY (i);

INSERT INTO a01 values(generate_series(0, 500));
INSERT INTO a02 values(generate_series(0, 500));
INSERT INTO a03 values(generate_series(0, 500));

\c test_tablenum_limit_02
CREATE EXTENSION diskquota;
CREATE SCHEMA s;
SELECT diskquota.set_schema_quota('s', '1 MB');

CREATE TABLE s.t1(i int) DISTRIBUTED BY (i);
INSERT INTO s.t1 SELECT generate_series(1,100000); -- expect failed. diskquota should works. activetable = 1
CREATE TABLE s.t2(i int) DISTRIBUTED BY (i);
INSERT INTO s.t2 SELECT generate_series(1,100000);

CREATE TABLE s.t3(i int) DISTRIBUTED BY (i);
INSERT INTO s.t3 SELECT generate_series(1,100000); -- should not crash. activetable = 3

DROP EXTENSION diskquota;

-- wait worker exit
\! sleep 1

\c contrib_regression
DROP DATABASE test_tablenum_limit_01;
DROP DATABASE test_tablenum_limit_02;

\! gpconfig -r diskquota.max_active_tables > /dev/null
\! gpstop -arf > /dev/null
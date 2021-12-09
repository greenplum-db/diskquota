CREATE ROLE r;
SELECT diskquota.set_role_quota('r', '10MB');
SET ROLE r;
select pg_sleep(5);

-- heap table
CREATE TABLE t1 AS SELECT generate_series(1, 100000000);
select pg_sleep(5);

-- temp table
CREATE TEMP TABLE t2 AS SELECT generate_series(1, 100000000);
select pg_sleep(5);

-- toast table
CREATE TABLE toast_table AS SELECT array(select * from generate_series(1,10000)) FROM generate_series(1, 100000);
select pg_sleep(5);

-- ao table
CREATE TABLE ao_table WITH(appendonly=true) AS SELECT generate_series(1, 100000000);
select pg_sleep(5);

-- aocs table
CREATE TABLE aocs_table WITH(appendonly=true, orientation=column) AS SELECT i, array(select * from generate_series(1,10000)) FROM generate_series(1, 100000) AS i;
select pg_sleep(5);

DROP TABLE IF EXISTS t1;
DROP TABLE IF EXISTS t2;
DROP TABLE IF EXISTS toast_table;
DROP TABLE IF EXISTS ao_table;
DROP TABLE IF EXISTS aocs_table;
RESET ROLE;
DROP ROLE r;
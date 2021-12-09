-- start_ignore
\! mkdir /tmp/cas_rolespc
-- end_ignore
DROP TABLESPACE IF EXISTS cas_rolespc;
CREATE TABLESPACE cas_rolespc LOCATION '/tmp/cas_rolespc';
CREATE ROLE r LOGIN SUPERUSER;
SELECT diskquota.set_role_tablespace_quota('r', 'cas_rolespc', '10MB');
SET default_tablespace = cas_rolespc;
SET ROLE r;
select pg_sleep(5);

-- heap table
CREATE TABLE t1 AS SELECT generate_series(1, 100000000);
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
RESET default_tablespace;
DROP ROLE r;
DROP TABLESPACE cas_rolespc;
\! rm -rf /tmp/cas_rolespc;
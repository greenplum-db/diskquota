\! gpconfig -c shared_preload_libraries -v '' > /dev/null 
\! gpstop -far > /dev/null
\c

CREATE ROLE test SUPERUSER;

SET ROLE test;

-- Create table "t" with diskquota disabled
CREATE TABLE t (i) AS SELECT generate_series(1, 100000);

\! gpconfig -c shared_preload_libraries -v 'diskquota'> /dev/null 
\! gpstop -far > /dev/null
\c

SET ROLE test;

-- Init table_size to include table "t"
SELECT diskquota.init_table_size_table();

SELECT tableid::regclass, size FROM diskquota.table_size
WHERE tableid = 't'::regclass AND segid = -1;

SELECT 't'::regclass::oid;

-- Ensure that table "t" is not active
SELECT diskquota.diskquota_fetch_table_stat(0, ARRAY[]::oid[])
FROM gp_dist_random('gp_id');

SELECT diskquota.set_role_quota(current_role, '1MB');

SELECT diskquota.wait_for_worker_new_epoch();

-- Expect that current role is in the blackmap 
SELECT rolname FROM pg_authid, diskquota.blackmap WHERE oid = target_oid;

SELECT diskquota.set_role_quota(current_role, '-1');

SELECT diskquota.wait_for_worker_new_epoch();

SELECT rolname FROM pg_authid, diskquota.blackmap WHERE oid = target_oid;

DROP TABLE t;

RESET ROLE;

DROP ROLE test;

CREATE TABLE t (i) AS SELECT generate_series(1, 100000);

SELECT diskquota.wait_for_worker_new_epoch();

SELECT tableid::regclass, size FROM diskquota.table_size
WHERE tableid = 't'::regclass AND segid = -1;

-- Ensure that t is not active
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

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION diskquota" to load this file. \quit

CREATE SCHEMA diskquota;

CREATE TABLE diskquota.quota_config(
  config jsonb
) WITH (appendonly=false);

-- TODO: replace diskquota.table_size with view and UDF
CREATE TABLE diskquota.table_size(
	tableid oid,
	size bigint,
	segid smallint,
	PRIMARY KEY(tableid, segid)
) WITH (appendonly=false) DISTRIBUTED BY (tableid, segid);

CREATE TABLE diskquota.state(
	state int,
	PRIMARY KEY(state)
) WITH (appendonly=false) DISTRIBUTED BY (state);

-- diskquota.quota_config AND diskquota.target is dump-able, other table can be generate on fly
SELECT pg_catalog.pg_extension_config_dump('diskquota.quota_config', '');
SELECT gp_segment_id, pg_catalog.pg_extension_config_dump('diskquota.quota_config', '') FROM gp_dist_random('gp_id');

CREATE TYPE diskquota.diskquota_active_table_type AS (
	"TABLE_OID" oid,
	"TABLE_SIZE" int8,
	"GP_SEGMENT_ID" smallint
);

CREATE TYPE diskquota.rejectmap_entry AS (
	quota_type integer,
	namespace_oid oid,
	owner_oid oid,
	database_oid oid,
	tablespace_oid oid,
	seg_exceeded boolean
);

CREATE TYPE diskquota.rejectmap_entry_detail AS (
	target_type text,
	namespace_oid oid,
	owner_oid oid,
	database_oid oid,
	tablespace_oid oid,
	relfilenode oid,
	seg_exceeded boolean,
	segid int
);

CREATE TYPE diskquota.relation_cache_detail AS (
	RELID oid,
	PRIMARY_TABLE_OID oid,
	AUXREL_NUM int,
	OWNEROID oid,
	NAMESPACEOID oid,
	BACKENDID int,
	SPCNODE oid,
	DBNODE oid,
	RELNODE oid,
	RELSTORAGE "char",
	AUXREL_OID oid[],
	RELAM oid
);

CREATE FUNCTION diskquota.set_schema_quota(text, text) RETURNS void STRICT AS '$libdir/diskquota-3.0.so' LANGUAGE C;
CREATE FUNCTION diskquota.set_role_quota(text, text) RETURNS void STRICT AS '$libdir/diskquota-3.0.so' LANGUAGE C;
CREATE FUNCTION diskquota.set_schema_tablespace_quota(text, text, text) RETURNS void STRICT AS '$libdir/diskquota-3.0.so' LANGUAGE C;
CREATE FUNCTION diskquota.set_role_tablespace_quota(text, text, text) RETURNS void STRICT AS '$libdir/diskquota-3.0.so' LANGUAGE C;
CREATE FUNCTION diskquota.set_per_segment_quota(text, float4) RETURNS void STRICT AS '$libdir/diskquota-3.0.so', 'set_tablespace_quota' LANGUAGE C;

CREATE FUNCTION diskquota.init_table_size_table() RETURNS void STRICT AS '$libdir/diskquota-3.0.so' LANGUAGE C;
CREATE FUNCTION diskquota.diskquota_fetch_table_stat(int4, oid[]) RETURNS setof diskquota.diskquota_active_table_type AS '$libdir/diskquota-3.0.so', 'diskquota_fetch_table_stat' LANGUAGE C VOLATILE;
CREATE FUNCTION diskquota.refresh_rejectmap(diskquota.rejectmap_entry[], oid[]) RETURNS void STRICT AS '$libdir/diskquota-3.0.so' LANGUAGE C;
CREATE FUNCTION diskquota.show_rejectmap() RETURNS setof diskquota.rejectmap_entry_detail AS '$libdir/diskquota-3.0.so', 'show_rejectmap' LANGUAGE C;
CREATE FUNCTION diskquota.pause() RETURNS void STRICT AS '$libdir/diskquota-3.0.so', 'diskquota_pause' LANGUAGE C;
CREATE FUNCTION diskquota.resume() RETURNS void STRICT AS '$libdir/diskquota-3.0.so', 'diskquota_resume' LANGUAGE C;
CREATE FUNCTION diskquota.show_worker_epoch() RETURNS bigint STRICT AS '$libdir/diskquota-3.0.so', 'show_worker_epoch' LANGUAGE C;
CREATE FUNCTION diskquota.wait_for_worker_new_epoch() RETURNS boolean STRICT AS '$libdir/diskquota-3.0.so', 'wait_for_worker_new_epoch' LANGUAGE C;
CREATE FUNCTION diskquota.status() RETURNS TABLE ("name" text, "status" text) STRICT AS '$libdir/diskquota-3.0.so', 'diskquota_status' LANGUAGE C;
CREATE FUNCTION diskquota.show_relation_cache() RETURNS setof diskquota.relation_cache_detail AS '$libdir/diskquota-3.0.so', 'show_relation_cache' LANGUAGE C;
CREATE FUNCTION diskquota.relation_size_local(reltablespace oid, relfilenode oid, relpersistence "char", relstorage "char", relam oid) RETURNS bigint STRICT AS '$libdir/diskquota-3.0.so', 'relation_size_local' LANGUAGE C;
CREATE FUNCTION diskquota.pull_all_table_size(OUT tableid oid, OUT size bigint, OUT segid smallint) RETURNS SETOF RECORD AS '$libdir/diskquota-3.0.so', 'pull_all_table_size' LANGUAGE C;

CREATE FUNCTION diskquota.relation_size(relation regclass) RETURNS bigint STRICT AS $$
       SELECT SUM(size)::bigint FROM (
               SELECT diskquota.relation_size_local(reltablespace, relfilenode, relpersistence,
		CASE WHEN EXISTS
	(SELECT FROM pg_catalog.pg_attribute WHERE attrelid = 'pg_class'::regclass AND attname = 'relstorage') THEN relstorage::"char" ELSE ''::"char" END,
		relam) AS size
               FROM gp_dist_random('pg_class') as relstorage WHERE oid = relation
               UNION ALL
               SELECT diskquota.relation_size_local(reltablespace, relfilenode, relpersistence,
		CASE WHEN EXISTS
	(SELECT FROM pg_catalog.pg_attribute WHERE attrelid = 'pg_class'::regclass AND attname = 'relstorage') THEN relstorage::"char" ELSE ''::"char" END,
		relam) AS size
               FROM pg_class as relstorage WHERE oid = relation
       ) AS t $$ LANGUAGE SQL;

CREATE FUNCTION diskquota.show_relation_cache_all_seg() RETURNS setof diskquota.relation_cache_detail AS $$
	WITH relation_cache AS (
		SELECT diskquota.show_relation_cache() AS a
		FROM  gp_dist_random('gp_id')
	)
	SELECT (a).* FROM relation_cache; $$ LANGUAGE SQL;

-- view part
CREATE VIEW diskquota.show_all_relation_view AS
WITH
  relation_cache AS (
    SELECT (f).* FROM diskquota.show_relation_cache() as f
  )
SELECT DISTINCT(oid), relowner, relnamespace, reltablespace from (
  SELECT relid as oid, owneroid as relowner, namespaceoid as relnamespace, spcnode as reltablespace FROM relation_cache
  UNION
  SELECT oid, relowner, relnamespace, reltablespace from pg_class
) as union_relation;

CREATE VIEW diskquota.show_fast_database_size_view AS
SELECT (
    (SELECT SUM(pg_relation_size(oid)) FROM pg_class WHERE oid <= 16384)
        +
    (SELECT SUM(size) FROM diskquota.table_size WHERE segid = -1)
) AS dbsize;

CREATE VIEW diskquota.rejectmap AS SELECT * FROM diskquota.show_rejectmap() AS BM;

-- quota config row type, contains all attributes in quota_config
CREATE TYPE diskquota.quota_config_row AS (
	quota_type int,
	db_oid oid,
	namespace_oid oid,
	owner_oid oid,
	tablespace_oid oid,
	quota_limit_mb bigint,
	segratio float4
);

-- quota config view
CREATE VIEW diskquota.show_quota_config_view AS 
WITH quota_config AS(
	SELECT
		jsonb_populate_recordset(
			null :: diskquota.quota_config_row,
			config -> 'quota_list'
		) AS quota_config_record
	FROM
		diskquota.quota_config
)
SELECT
	(quota_config_record).*
FROM
	quota_config;

CREATE VIEW diskquota.show_fast_schema_quota_view AS
WITH quota_usage AS (
	SELECT
		relnamespace,
		SUM(size) AS total_size
	FROM
		diskquota.table_size,
		diskquota.show_all_relation_view
	WHERE
		tableid = diskquota.show_all_relation_view.oid
		AND segid = -1
	GROUP BY
		relnamespace
)
SELECT
	nspname AS schema_name,
	namespace_oid AS schema_oid,
	quota_limit_mb AS quota_in_mb,
	COALESCE(total_size, 0) AS nspsize_in_bytes
FROM
	diskquota.show_quota_config_view
	JOIN pg_namespace ON namespace_oid = pg_namespace.oid
	LEFT OUTER JOIN quota_usage ON pg_namespace.oid = relnamespace
WHERE
	quota_type = 0;  -- NAMESPACE_QUOTA

CREATE VIEW diskquota.show_fast_role_quota_view AS
WITH quota_usage AS (
	SELECT
		relowner,
		SUM(size) AS total_size
	FROM
		diskquota.table_size,
		diskquota.show_all_relation_view
	WHERE
		tableid = diskquota.show_all_relation_view.oid
		AND segid = -1
	GROUP BY
		relowner
)
SELECT
	rolname AS role_name,
	owner_oid AS role_oid,
	quota_limit_mb AS quota_in_mb,
	COALESCE(total_size, 0) AS rolsize_in_bytes
FROM
	diskquota.show_quota_config_view
	JOIN pg_roles ON owner_oid = pg_roles.oid
	LEFT OUTER JOIN quota_usage ON pg_roles.oid = relowner
WHERE
	quota_type = 1;  -- ROLE_QUOTA


CREATE VIEW diskquota.show_fast_schema_tablespace_quota_view AS
WITH default_tablespace AS (
	SELECT
		dattablespace
	FROM
		pg_database
	WHERE
		datname = current_database()
),
quota_usage AS (
	SELECT
		relnamespace,
		CASE
			WHEN reltablespace = 0 THEN dattablespace
			ELSE reltablespace
		END AS reltablespace,
		SUM(size) AS total_size
	FROM
		diskquota.table_size,
		diskquota.show_all_relation_view,
		default_tablespace
	WHERE
		tableid = diskquota.show_all_relation_view.oid
		AND segid = -1
	GROUP BY
		relnamespace,
		reltablespace,
		dattablespace
)
SELECT
	nspname AS schema_name,
	namespace_oid AS schema_oid,
	spcname AS tablespace_name,
	tablespace_oid AS tablespace_oid,
	quota_limit_mb AS quota_in_mb,
	COALESCE(total_size, 0) AS nspsize_tablespace_in_bytes
FROM
	diskquota.show_quota_config_view
	JOIN pg_namespace ON namespace_oid = pg_namespace.oid
	JOIN pg_tablespace ON tablespace_oid = pg_tablespace.oid
	LEFT OUTER JOIN quota_usage ON pg_namespace.oid = relnamespace
	AND pg_tablespace.oid = reltablespace
WHERE
	quota_type = 2;  -- NAMESPACE_TABLESPACE_QUOTA

CREATE VIEW diskquota.show_fast_role_tablespace_quota_view AS
WITH default_tablespace AS (
	SELECT
		dattablespace
	FROM
		pg_database
	WHERE
		datname = current_database()
),
quota_usage AS (
	SELECT
		relowner,
		CASE
			WHEN reltablespace = 0 THEN dattablespace
			ELSE reltablespace
		END AS reltablespace,
		SUM(size) AS total_size
	FROM
		diskquota.table_size,
		diskquota.show_all_relation_view,
		default_tablespace
	WHERE
		tableid = diskquota.show_all_relation_view.oid
		AND segid = -1
	GROUP BY
		relowner,
		reltablespace,
		dattablespace
)
SELECT
	rolname AS role_name,
	owner_oid AS role_oid,
	spcname AS tablespace_name,
	tablespace_oid AS tablespace_oid,
	quota_limit_mb AS quota_in_mb,
	COALESCE(total_size, 0) AS rolsize_tablespace_in_bytes
FROM
	diskquota.show_quota_config_view
	JOIN pg_roles ON owner_oid = pg_roles.oid
	JOIN pg_tablespace ON tablespace_oid = pg_tablespace.oid
	LEFT OUTER JOIN quota_usage ON pg_roles.oid = relowner
	AND pg_tablespace.oid = reltablespace
WHERE
	quota_type = 3;  -- ROLE_TABLESPACE_QUOTA

CREATE VIEW diskquota.show_segment_ratio_quota_view AS
SELECT
	spcname as tablespace_name,
	pg_tablespace.oid as tablespace_oid,
	segratio as per_seg_quota_ratio
FROM
	diskquota.show_quota_config_view
	JOIN pg_tablespace ON tablespace_oid = pg_tablespace.oid
WHERE
	quota_type = 4;  -- TABLESPACE_QUOTA

-- view end

-- re-dispatch pause status to false. in case user pause-drop-recreate.
-- refer to see test case 'test_drop_after_pause'
SELECT FROM diskquota.resume();

-- for test
CREATE OR REPLACE FUNCTION diskquota.test_send_message_loop(int) RETURNS boolean STRICT AS '$libdir/diskquota-3.0.so' LANGUAGE C;

--- Starting the worker has to be the last step.
CREATE FUNCTION diskquota.diskquota_start_worker() RETURNS void STRICT AS '$libdir/diskquota-3.0.so' LANGUAGE C;
SELECT diskquota.diskquota_start_worker();
DROP FUNCTION diskquota.diskquota_start_worker();

-- table part
ALTER TABLE diskquota.quota_config ADD COLUMN segratio float4 DEFAULT -1;

CREATE TABLE diskquota.target (
	quotatype int, -- REFERENCES disquota.quota_config.quotatype,
	primaryOid oid,
	tablespaceOid oid, -- REFERENCES pg_tablespace.oid,
	PRIMARY KEY (primaryOid, tablespaceOid, quotatype)
);

ALTER TABLE diskquota.table_size DROP CONSTRAINT table_size_pkey;
ALTER TABLE diskquota.table_size ADD COLUMN segid smallint DEFAULT -1; -- segid = coordinator means table size in cluster level
ALTER TABLE diskquota.table_size SET DISTRIBUTED by (tableid, segid);
ALTER TABLE diskquota.table_size ADD PRIMARY KEY (tableid, segid);

SELECT pg_catalog.pg_extension_config_dump('diskquota.target', '');
SELECT gp_segment_id, pg_catalog.pg_extension_config_dump('diskquota.target', '') from gp_dist_random('gp_id');
-- table part end

-- type define
ALTER TYPE diskquota.diskquota_active_table_type ADD ATTRIBUTE (
	"GP_SEGMENT_ID" smallint
);

CREATE TYPE diskquota.blackmap_entry AS (
	target_oid oid,
	database_oid oid,
	tablespace_oid oid,
	target_type integer,
	seg_exceeded boolean
);

CREATE TYPE diskquota.blackmap_entry_detail AS (
	target_type text,
	target_oid oid,
	database_oid oid,
	tablespace_oid oid,
	seg_exceeded boolean,
	dbnode oid,
	spcnode oid,
	relnode oid,
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
	AUXREL_OID oid[]
);
-- type define end

-- UDF
ALTER FUNCTION diskquota.set_schema_quota(text, text) RETURNS void STRICT AS '$libdir/diskquota.so' LANGUAGE C;
ALTER FUNCTION diskquota.set_role_quota(text, text) RETURNS void STRICT AS '$libdir/diskquota.so' LANGUAGE C;
ALTER FUNCTION diskquota.init_table_size_table() RETURNS void STRICT AS '$libdir/diskquota.so' LANGUAGE C;
ALTER FUNCTION diskquota.diskquota_fetch_table_stat(int4, oid[]) RETURNS setof diskquota.diskquota_active_table_type AS '$libdir/diskquota.so', 'diskquota_fetch_table_stat' LANGUAGE C VOLATILE;

DROP FUNCTION diskquota.update_diskquota_db_list(oid, int4);

CREATE FUNCTION diskquota.set_schema_tablespace_quota(text, text, text) RETURNS void STRICT AS '$libdir/diskquota-2.0.so' LANGUAGE C;
CREATE FUNCTION diskquota.set_role_tablespace_quota(text, text, text) RETURNS void STRICT AS '$libdir/diskquota-2.0.so' LANGUAGE C;
CREATE FUNCTION diskquota.set_per_segment_quota(text, float4) RETURNS void STRICT AS '$libdir/diskquota-2.0.so' LANGUAGE C;
CREATE FUNCTION diskquota.refresh_blackmap(diskquota.blackmap_entry[], oid[]) RETURNS void STRICT AS '$libdir/diskquota-2.0.so' LANGUAGE C;
CREATE FUNCTION diskquota.show_blackmap() RETURNS setof diskquota.blackmap_entry_detail AS '$libdir/diskquota-2.0.so', 'show_blackmap' LANGUAGE C;
CREATE FUNCTION diskquota.pause() RETURNS void STRICT AS '$libdir/diskquota-2.0.so', 'diskquota_pause' LANGUAGE C;
CREATE FUNCTION diskquota.resume() RETURNS void STRICT AS '$libdir/diskquota-2.0.so', 'diskquota_resume' LANGUAGE C;
CREATE FUNCTION diskquota.show_worker_epoch() RETURNS bigint STRICT AS '$libdir/diskquota-2.0.so', 'show_worker_epoch' LANGUAGE C;
CREATE FUNCTION diskquota.wait_for_worker_new_epoch() RETURNS boolean STRICT AS '$libdir/diskquota-2.0.so', 'wait_for_worker_new_epoch' LANGUAGE C;
CREATE FUNCTION diskquota.status() RETURNS TABLE ("name" text, "status" text) STRICT AS '$libdir/diskquota-2.0.so', 'diskquota_status' LANGUAGE C;
CREATE FUNCTION diskquota.show_relation_cache() RETURNS setof diskquota.relation_cache_detail AS '$libdir/diskquota-2.0.so', 'show_relation_cache' LANGUAGE C;
CREATE FUNCTION diskquota.relation_size_local(
	reltablespace oid,
	relfilenode oid,
	relpersistence "char",
	relstorage "char")
RETURNS bigint STRICT AS '$libdir/diskquota-2.0.so', 'relation_size_local' LANGUAGE C;
CREATE FUNCTION diskquota.relation_size(relation regclass) RETURNS bigint STRICT AS $$
	SELECT sum(size)::bigint FROM (
		SELECT diskquota.relation_size_local(reltablespace, relfilenode, relpersistence, relstorage) AS size
		FROM gp_dist_random('pg_class') WHERE oid = relation
		UNION ALL
		SELECT diskquota.relation_size_local(reltablespace, relfilenode, relpersistence, relstorage) AS size
		FROM pg_class WHERE oid = relation
	) AS t $$ LANGUAGE SQL;

CREATE FUNCTION diskquota.show_relation_cache_all_seg() RETURNS setof diskquota.relation_cache_detail AS $$
	WITH relation_cache AS (
		SELECT diskquota.show_relation_cache() AS a
		FROM  gp_dist_random('gp_id')
	)
	SELECT (a).* FROM relation_cache; $$ LANGUAGE SQL;
-- UDF end

-- views
CREATE VIEW diskquota.blackmap AS SELECT * FROM diskquota.show_blackmap() AS BM;

CREATE VIEW diskquota.show_fast_schema_tablespace_quota_view AS
select pgns.nspname as schema_name, pgc.relnamespace as schema_oid, pgsp.spcname as tablespace_name, pgc.reltablespace as tablespace_oid, qc.quotalimitMB as quota_in_mb, sum(ts.size) as nspsize_tablespace_in_bytes
from diskquota.table_size as ts,
	pg_class as pgc,
	diskquota.quota_config as qc,
	pg_namespace as pgns,
	pg_tablespace as pgsp,
	diskquota.target as t
where ts.tableid = pgc.oid and qc.targetoid = pgc.relnamespace and pgns.oid = pgc.relnamespace and pgsp.oid = pgc.reltablespace and qc.quotatype=2 and qc.targetoid=t.primaryoid and t.tablespaceoid=pgc.reltablespace and ts.segid=-1
group by relnamespace, reltablespace, qc.quotalimitMB, pgns.nspname, pgsp.spcname
order by pgns.nspname, pgsp.spcname;

CREATE VIEW diskquota.show_fast_role_tablespace_quota_view AS
select pgr.rolname as role_name, pgc.relowner as role_oid, pgsp.spcname as tablespace_name, pgc.reltablespace as tablespace_oid, qc.quotalimitMB as quota_in_mb, sum(ts.size) as rolsize_tablespace_in_bytes
from diskquota.table_size as ts,
	pg_class as pgc,
	diskquota.quota_config as qc,
	pg_roles as pgr,
	pg_tablespace as pgsp,
	diskquota.target as t
WHERE pgc.relowner = qc.targetoid and pgc.relowner = pgr.oid and ts.tableid = pgc.oid and pgsp.oid = pgc.reltablespace and qc.quotatype=3 and qc.targetoid=t.primaryoid and t.tablespaceoid=pgc.reltablespace and ts.segid=-1
GROUP BY pgc.relowner, reltablespace, pgr.rolname, pgsp.spcname, qc.quotalimitMB;
-- views end

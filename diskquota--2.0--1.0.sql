-- TODO check if worker should not refresh, current lib should be diskquota.so

-- views
DROP VIEW diskquota.blackmap;
DROP VIEW diskquota.show_fast_schema_tablespace_quota_view;
DROP VIEW diskquota.show_fast_role_tablespace_quota_view;

/* ALTER */ CREATE OR REPLACE VIEW diskquota.show_fast_database_size_view AS
select (
    (select sum(pg_relation_size(oid)) from pg_class where oid <= 16384)
        +
    (select sum(size) from diskquota.table_size)
) AS dbsize;

/* ALTER */ CREATE OR REPLACE VIEW diskquota.show_fast_schema_quota_view AS
select pgns.nspname as schema_name, pgc.relnamespace as schema_oid, qc.quotalimitMB as quota_in_mb, sum(ts.size) as nspsize_in_bytes
from diskquota.table_size as ts,
   pg_class as pgc,
   diskquota.quota_config as qc,
   pg_namespace as pgns
where ts.tableid = pgc.oid and qc.targetoid = pgc.relnamespace and pgns.oid = pgc.relnamespace
group by relnamespace, qc.quotalimitMB, pgns.nspname
order by pgns.nspname;

/* ALTER */ CREATE OR REPLACE VIEW diskquota.show_fast_role_quota_view AS
select pgr.rolname as role_name, pgc.relowner as role_oid, qc.quotalimitMB as quota_in_mb, sum(ts.size) as rolsize_in_bytes
from diskquota.table_size as ts,
   pg_class as pgc,
   diskquota.quota_config as qc,
   pg_roles as pgr
WHERE pgc.relowner = qc.targetoid and pgc.relowner = pgr.oid and ts.tableid = pgc.oid
GROUP BY pgc.relowner, pgr.rolname, qc.quotalimitMB;
-- views part end

-- UDF
-- TODO find a way to use ALTER FUNCTION
/* ALTER */ CREATE OR REPLACE FUNCTION diskquota.set_schema_quota(text, text) RETURNS void STRICT AS '$libdir/diskquota.so' LANGUAGE C;
/* ALTER */ CREATE OR REPLACE FUNCTION diskquota.set_role_quota(text, text) RETURNS void STRICT AS '$libdir/diskquota.so' LANGUAGE C;
/* 1.0--2.0 can not drop this UDF */ CREATE OR REPLACE FUNCTION diskquota.update_diskquota_db_list(oid, int4) RETURNS void STRICT AS '$libdir/diskquota.so' LANGUAGE C;
-- TODO find a way to run it in Postgresql 9.4 ALTER FUNCTION diskquota.update_diskquota_db_list(oid, int4) DEPENDS ON EXTENSION diskquota;
/* ALTER */ CREATE OR REPLACE FUNCTION diskquota.init_table_size_table() RETURNS void STRICT AS '$libdir/diskquota.so' LANGUAGE C;
/* ALTER */ CREATE OR REPLACE FUNCTION diskquota.diskquota_fetch_table_stat(int4, oid[]) RETURNS setof diskquota.diskquota_active_table_type AS '$libdir/diskquota.so', 'diskquota_fetch_table_stat' LANGUAGE C VOLATILE;

DROP FUNCTION diskquota.set_schema_tablespace_quota(text, text, text);
DROP FUNCTION diskquota.set_role_tablespace_quota(text, text, text);
DROP FUNCTION diskquota.set_per_segment_quota(text, float4);
DROP FUNCTION diskquota.refresh_blackmap(diskquota.blackmap_entry[], oid[]);
DROP FUNCTION diskquota.show_blackmap();
DROP FUNCTION diskquota.pause();
DROP FUNCTION diskquota.resume();
DROP FUNCTION diskquota.show_worker_epoch();
DROP FUNCTION diskquota.wait_for_worker_new_epoch();
DROP FUNCTION diskquota.status();
DROP FUNCTION diskquota.show_relation_cache();
DROP FUNCTION diskquota.relation_size_local(
	reltablespace oid,
	relfilenode oid,
	relpersistence "char",
	relstorage "char");
DROP FUNCTION diskquota.relation_size(relation regclass);
DROP FUNCTION diskquota.show_relation_cache_all_seg();
-- UDF end

-- table part
-- clean up schema_tablespace quota and rolsize_tablespace quota
DELETE FROM diskquota.quota_config WHERE quotatype = 2 or quotatype = 3;

DROP TABLE diskquota.target;

ALTER TABLE diskquota.quota_config DROP COLUMN segratio;

ALTER TABLE diskquota.table_size SET WITH (REORGANIZE=true) DISTRIBUTED BY (tableid);
ALTER TABLE diskquota.table_size DROP CONSTRAINT table_size_pkey;
-- clean up pre segments size information, 1.0 do not has this feature
DELETE FROM diskquota.table_size WHERE segid != -1;
ALTER TABLE diskquota.table_size ADD PRIMARY KEY (tableid);
ALTER TABLE diskquota.table_size DROP COLUMN segid;
-- table part end

-- type part
ALTER TYPE diskquota.diskquota_active_table_type DROP ATTRIBUTE "GP_SEGMENT_ID";
DROP TYPE diskquota.blackmap_entry;
DROP TYPE diskquota.blackmap_entry_detail;
DROP TYPE diskquota.relation_cache_detail;
-- type part end

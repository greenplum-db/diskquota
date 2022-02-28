-- table part
-- clean up pre segments size information, 1.0 do not has this feature
DELETE FROM diskquota.table_size WHERE segid != -1;
-- clean up schema_tablespace quota and rolsize_tablespace quota
DELETE FROM diskquota.quota_config WHERE quotatype = 2 or quotatype = 3;

DROP TABLE diskquota.target;

ALTER TABLE diskquota.quota_config DROP COLUMN segratio;

ALTER TABLE diskquota.table_size DROP CONSTRAINT table_size_pkey;
ALTER TABLE diskquota.table_size ADD PRIMARY KEY (tableid);
ALTER TABLE diskquota.table_size SET DISTRIBUTED BY (tableid);
ALTER TABLE diskquota.table_size DROP COLUMN segid;
-- table part end

CREATE TYPE diskquota.diskquota_active_table_type AS ("TABLE_OID" oid,  "TABLE_SIZE" int8);
CREATE OR REPLACE FUNCTION diskquota.diskquota_fetch_table_stat(int4, oid[]) RETURNS setof diskquota.diskquota_active_table_type
AS '$libdir/diskquota.so', 'diskquota_fetch_table_stat'
LANGUAGE C VOLATILE;

-- UDF
CREATE FUNCTION diskquota.update_diskquota_db_list(oid, int4) RETURNS void STRICT AS '$libdir/diskquota.so' LANGUAGE C;

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

-- type
ALTER TYPE diskquota.diskquota_active_table_type DROP ATTRIBUTE (
	"GP_SEGMENT_ID" smallint
);

DROP TYPE diskquota.blackmap_entry;
DROP TYPE diskquota.blackmap_entry_detail;
DROP TYPE diskquota.relation_cache_detail;
-- type end

-- views
DROP VIEW diskquota.blackmap;
DROP VIEW diskquota.show_fast_schema_tablespace_quota_view;
DROP VIEW diskquota.show_fast_role_tablespace_quota_view;

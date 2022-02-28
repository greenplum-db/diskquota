-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION diskquota" to load this file. \quit

CREATE SCHEMA diskquota;

-- Configuration table
CREATE TABLE diskquota.quota_config(
    targetOid oid,
    quotatype int,
    quotalimitMB int8,
    PRIMARY KEY(targetOid, quotatype)
) DISTRIBUTED BY (targetOid, quotatype);

CREATE TABLE diskquota.table_size(
    tableid oid,
    size bigint,
    PRIMARY KEY(tableid)
);

CREATE TABLE diskquota.state(
    state int,
    PRIMARY KEY(state)
) DISTRIBUTED BY (state);

-- only diskquota.quota_config is dump-able, other table can be generate on fly
SELECT pg_catalog.pg_extension_config_dump('diskquota.quota_config', '');
SELECT gp_segment_id, pg_catalog.pg_extension_config_dump('diskquota.quota_config', '') from gp_dist_random('gp_id');

CREATE TYPE diskquota.diskquota_active_table_type AS (
	"TABLE_OID" oid,
	"TABLE_SIZE" int8
);

CREATE FUNCTION diskquota.set_schema_quota(text, text) RETURNS void STRICT AS '$libdir/diskquota.so' LANGUAGE C;
CREATE FUNCTION diskquota.set_role_quota(text, text) RETURNS void STRICT AS '$libdir/diskquota.so' LANGUAGE C;
CREATE FUNCTION diskquota.update_diskquota_db_list(oid, int4) RETURNS void STRICT AS '$libdir/diskquota.so' LANGUAGE C;
CREATE FUNCTION diskquota.init_table_size_table() RETURNS void STRICT AS '$libdir/diskquota.so' LANGUAGE C;
CREATE FUNCTION diskquota.diskquota_fetch_table_stat(int4, oid[]) RETURNS setof diskquota.diskquota_active_table_type AS '$libdir/diskquota.so', 'diskquota_fetch_table_stat' LANGUAGE C VOLATILE;

CREATE VIEW diskquota.show_fast_schema_quota_view AS
select pgns.nspname as schema_name, pgc.relnamespace as schema_oid, qc.quotalimitMB as quota_in_mb, sum(ts.size) as nspsize_in_bytes
from diskquota.table_size as ts,
	pg_class as pgc,
	diskquota.quota_config as qc,
	pg_namespace as pgns
where ts.tableid = pgc.oid and qc.targetoid = pgc.relnamespace and pgns.oid = pgc.relnamespace
group by relnamespace, qc.quotalimitMB, pgns.nspname
order by pgns.nspname;

CREATE VIEW diskquota.show_fast_role_quota_view AS
select pgr.rolname as role_name, pgc.relowner as role_oid, qc.quotalimitMB as quota_in_mb, sum(ts.size) as rolsize_in_bytes
from diskquota.table_size as ts,
	pg_class as pgc,
	diskquota.quota_config as qc,
	pg_roles as pgr
WHERE pgc.relowner = qc.targetoid and pgc.relowner = pgr.oid and ts.tableid = pgc.oid
GROUP BY pgc.relowner, pgr.rolname, qc.quotalimitMB;

CREATE VIEW diskquota.show_fast_database_size_view AS
select (
	(select sum(pg_relation_size(oid)) from pg_class where oid <= 16384)
		+
	(select sum(size) from diskquota.table_size)
) AS dbsize;

-- prepare to boot
INSERT INTO diskquota.state SELECT (count(relname) = 0)::int  FROM pg_class AS c, pg_namespace AS n WHERE c.oid > 16384 and relnamespace = n.oid and nspname != 'diskquota';

CREATE FUNCTION diskquota.diskquota_start_worker() RETURNS void STRICT AS '$libdir/diskquota.so' LANGUAGE C;
SELECT diskquota.diskquota_start_worker();
DROP FUNCTION diskquota.diskquota_start_worker();

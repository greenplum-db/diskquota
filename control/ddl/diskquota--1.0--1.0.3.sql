\echo use "alter extension diskquota update to '1.0.3'" to load this file. \quit

SELECT gp_segment_id, pg_catalog.pg_extension_config_dump('diskquota.quota_config', '') from gp_dist_random('gp_id');

CREATE FUNCTION diskquota.update_diskquota_db_list(oid, int4)
RETURNS void STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;

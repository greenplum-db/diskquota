CREATE TYPE diskquota.db_status AS (
	"dbid" oid,
	"status" int8,
	"epoch" int8 ,
	"paused" bool,
	"datname" text
);
CREATE FUNCTION diskquota.db_status() RETURNS setof diskquota.db_status AS '$libdir/diskquota-2.1.so',  'db_status' LANGUAGE C VOLATILE;

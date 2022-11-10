-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION diskquota_test" to load this file. \quit

CREATE FUNCTION diskquota.wait(sql text) RETURNS bool
AS $$
DECLARE
res bool := false;
count integer := 10;
BEGIN
    WHILE count > 0 LOOP
        EXECUTE  sql into res;
        IF res THEN
                RETURN res;
        ELSE
                count = count - 1;
                EXECUTE 'select pg_sleep(1);';
        END IF;
    END LOOP;
    RETURN res;
END;
$$ LANGUAGE plpgsql;

CREATE TYPE diskquota.db_status AS (
        "dbid" oid,
        "datname" text,
        "status" text,
        "epoch" int8,
        "paused" bool
);
CREATE FUNCTION diskquota.db_status() RETURNS setof diskquota.db_status AS '$libdir/diskquota-2.1.so',  'db_status' LANGUAGE C VOLATILE;

CREATE FUNCTION diskquota.cur_db_status() RETURNS diskquota.db_status AS $$
SELECT * from diskquota.db_status() where datname = current_database();
$$ LANGUAGE SQL;

CREATE FUNCTION diskquota.check_cur_db_status(text) RETURNS boolean AS $$
SELECT $1 = db.status from diskquota.db_status() as db where db.datname = current_database();
$$ LANGUAGE SQL;

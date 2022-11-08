CREATE DATABASE test_readiness_logged;
\c test_readiness_logged

CREATE TABLE t (i int) DISTRIBUTED BY (i);

CREATE EXTENSION diskquota;
SELECT diskquota.wait('SELECT diskquota.check_cur_db_status(''UNREADY'');');

SELECT count(*) FROM gp_toolkit.gp_log_database
WHERE logmessage = '[diskquota] diskquota is not ready';

\! gpstop -raf > /dev/null
\c
SELECT diskquota.wait('SELECT diskquota.check_cur_db_status(''UNREADY'');');

SELECT count(*) FROM gp_toolkit.gp_log_database
WHERE logmessage = '[diskquota] diskquota is not ready';

DROP EXTENSION diskquota;

\c contrib_regression
DROP DATABASE test_readiness_logged;

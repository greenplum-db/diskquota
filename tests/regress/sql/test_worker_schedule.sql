--start_ignore
\! gpconfig -c diskquota.naptime -v 1;
\! gpconfig -c diskquota.max_workers -v 1;
\! gpstop -u;

CREATE DATABASE t1;
CREATE DATABASE t2;
CREATE DATABASE t3;
CREATE DATABASE t4;
--end_ignore

\c t1
CREATE EXTENSION diskquota;
show diskquota.naptime;
\c t2
CREATE EXTENSION diskquota;

\c t1
SELECT diskquota.wait_for_worker_new_epoch();
\c t2
SELECT diskquota.wait_for_worker_new_epoch();

--start_ignore
\! gpconfig -c diskquota.naptime -v 1;
\! gpconfig -c diskquota.max_workers -v 3;
\! gpstop -ari
--end_ignore

\c t1
SELECT diskquota.wait_for_worker_new_epoch();
\c t2
SELECT diskquota.wait_for_worker_new_epoch();


\c t3
CREATE EXTENSION diskquota;

\c t1
SELECT diskquota.wait_for_worker_new_epoch();
\c t2
SELECT diskquota.wait_for_worker_new_epoch();
\c t3
SELECT diskquota.wait_for_worker_new_epoch();

\c t4
CREATE EXTENSION diskquota;

\c t1
SELECT diskquota.wait_for_worker_new_epoch();
\c t2
SELECT diskquota.wait_for_worker_new_epoch();
\c t3
SELECT diskquota.wait_for_worker_new_epoch();
\c t4
SELECT diskquota.wait_for_worker_new_epoch();
--start_ignore
\c t1
DROP EXTENSION diskquota;
\c t2
DROP EXTENSION diskquota;
\c t3
DROP EXTENSION diskquota;
\c t4
DROP EXTENSION diskquota;

\c contrib_regression
DROP DATABASE t1;
DROP DATABASE t2;
DROP DATABASE t3;
DROP DATABASE t4;
\! gpconfig -r diskquota.worker_timeout;
\! gpstop -u;
--end_ignore

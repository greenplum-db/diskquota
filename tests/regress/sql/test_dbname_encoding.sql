-- create a database with non-ascii characters
CREATE DATABASE 数据库1;

\c 数据库1

CREATE EXTENSION diskquota;
SELECT diskquota.wait_for_worker_new_epoch();

\! ps -ef | grep postgres | grep "\[diskquota] 数据库1" | grep -v grep | wc -l

DROP EXTENSION diskquota;
\c contrib_regression
DROP DATABASE 数据库1;
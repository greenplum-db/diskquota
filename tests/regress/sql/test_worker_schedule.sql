--start_ignore
\! gpconfig -c diskquota.naptime -v 1;
\! gpconfig -c diskquota.max_workers -v 1;
\! gpstop -u;

CREATE DATABASE t1;
CREATE DATABASE t2;
CREATE DATABASE t3;
CREATE DATABASE t4;
CREATE DATABASE t5;
CREATE DATABASE t6;
CREATE DATABASE t7;
--end_ignore

\c t1
CREATE EXTENSION diskquota;
CREATE TABLE f1(a int);
INSERT into f1 SELECT generate_series(0,1000);
SELECT diskquota.wait_for_worker_new_epoch();
SELECT tableid::regclass, size, segid FROM diskquota.table_size WHERE tableid = 'f1'::regclass and segid = -1;

\c t2
CREATE EXTENSION diskquota;
CREATE TABLE f2(a int);
INSERT into f2 SELECT generate_series(0,1000);
SELECT diskquota.wait_for_worker_new_epoch();
SELECT tableid::regclass, size, segid FROM diskquota.table_size WHERE tableid = 'f2'::regclass and segid = -1;

--start_ignore
\! gpconfig -c diskquota.naptime -v 1;
\! gpconfig -c diskquota.max_workers -v 3;
\! gpstop -u;
--end_ignore

\c t3
CREATE EXTENSION diskquota;
CREATE TABLE f3(a int);
INSERT into f3 SELECT generate_series(0,1000);
SELECT diskquota.wait_for_worker_new_epoch();
SELECT tableid::regclass, size, segid FROM diskquota.table_size WHERE tableid = 'f3'::regclass and segid = -1;

\c t4
CREATE EXTENSION diskquota;
CREATE TABLE f4(a int);
INSERT into f4 SELECT generate_series(0,1000);
SELECT diskquota.wait_for_worker_new_epoch();
SELECT tableid::regclass, size, segid FROM diskquota.table_size WHERE tableid = 'f4'::regclass and segid = -1;

\c t5
CREATE EXTENSION diskquota;
CREATE TABLE f5(a int);
INSERT into f5 SELECT generate_series(0,1000);
SELECT diskquota.wait_for_worker_new_epoch();
SELECT tableid::regclass, size, segid FROM diskquota.table_size WHERE tableid = 'f5'::regclass and segid = -1;

\c t6
CREATE EXTENSION diskquota;
CREATE TABLE f6(a int);
INSERT into f6 SELECT generate_series(0,1000);
SELECT diskquota.wait_for_worker_new_epoch();
SELECT tableid::regclass, size, segid FROM diskquota.table_size WHERE tableid = 'f6'::regclass and segid = -1;

\c t7
CREATE EXTENSION diskquota;
CREATE TABLE f7(a int);
INSERT into f7 SELECT generate_series(0,1000);
SELECT diskquota.wait_for_worker_new_epoch();
SELECT tableid::regclass, size, segid FROM diskquota.table_size WHERE tableid = 'f7'::regclass and segid = -1;

\c t1
INSERT into f1 SELECT generate_series(0,100000);
SELECT diskquota.wait_for_worker_new_epoch();
SELECT tableid::regclass, size, segid FROM diskquota.table_size WHERE tableid = 'f1'::regclass and segid = -1;

\c t2
INSERT into f2 SELECT generate_series(0,100000);
SELECT diskquota.wait_for_worker_new_epoch();
SELECT tableid::regclass, size, segid FROM diskquota.table_size WHERE tableid = 'f2'::regclass and segid = -1;

\c t3
INSERT into f3 SELECT generate_series(0,100000);
SELECT diskquota.wait_for_worker_new_epoch();
SELECT tableid::regclass, size, segid FROM diskquota.table_size WHERE tableid = 'f3'::regclass and segid = -1;

\c t4
INSERT into f4 SELECT generate_series(0,100000);
SELECT diskquota.wait_for_worker_new_epoch();
SELECT tableid::regclass, size, segid FROM diskquota.table_size WHERE tableid = 'f4'::regclass and segid = -1;

\c t5
INSERT into f5 SELECT generate_series(0,100000);
SELECT diskquota.wait_for_worker_new_epoch();
SELECT tableid::regclass, size, segid FROM diskquota.table_size WHERE tableid = 'f5'::regclass and segid = -1;

\c t6
INSERT into f6 SELECT generate_series(0,100000);
SELECT diskquota.wait_for_worker_new_epoch();
SELECT tableid::regclass, size, segid FROM diskquota.table_size WHERE tableid = 'f6'::regclass and segid = -1;

\c t7
INSERT into f7 SELECT generate_series(0,100000);
SELECT diskquota.wait_for_worker_new_epoch();
SELECT tableid::regclass, size, segid FROM diskquota.table_size WHERE tableid = 'f7'::regclass and segid = -1;

--start_ignore
\c t1
DROP EXTENSION diskquota;
\c t2
DROP EXTENSION diskquota;
\c t3
DROP EXTENSION diskquota;
\c t4
DROP EXTENSION diskquota;
\c t5
DROP EXTENSION diskquota;
\c t6
DROP EXTENSION diskquota;
\c t7
DROP EXTENSION diskquota;

\c contrib_regression
DROP DATABASE t1;
DROP DATABASE t2;
DROP DATABASE t3;
DROP DATABASE t4;
DROP DATABASE t5;
DROP DATABASE t6;
DROP DATABASE t7;
\! gpconfig -r diskquota.worker_timeout;
\! gpconfig -r diskquota.naptime;
\! gpconfig -r diskquota.max_workers;
\! gpstop -u;
--end_ignore

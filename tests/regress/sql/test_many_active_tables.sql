CREATE TABLE t1 (pk int, val int)
DISTRIBUTED BY (pk)
PARTITION BY RANGE (pk) (START (1) END (1000) EVERY (1));

INSERT INTO t1 
SELECT pk, val
FROM generate_series(1, 10000) AS val, generate_series(1, 999) AS pk;

SELECT pg_sleep(5);

SELECT count(*) >= 999 FROM diskquota.table_size WHERE size > 0;

DROP TABLE t1;

SELECT pg_sleep(5);

SELECT count(*) < 999 FROM diskquota.table_size WHERE size > 0;

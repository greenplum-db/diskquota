-- start_ignore
\! mkdir -p /tmp/spc1
-- end_ignore

CREATE SCHEMA s1;
CREATE role r1 NOLOGIN;
CREATE TABLESPACE spc1 LOCATION '/tmp/spc1';


SELECT diskquota.set_schema_quota('s1', '1MB');
SELECT diskquota.set_role_quota('r1', '2MB');
SELECT diskquota.set_schema_tablespace_quota('s1', 'spc1', '3MB');
SELECT diskquota.set_role_tablespace_quota('r1', 'spc1', '4MB');
SELECT diskquota.set_per_segment_quota('spc1', 0.1);

\c diskquota
SELECT * from diskquota.quota_config;

\c contrib_regression
DROP SCHEMA s1;
DROP role r1;
DROP TABLESPACE spc1;
-- start_ignore
\! rm -rf /tmp/spc1
-- end_ignore

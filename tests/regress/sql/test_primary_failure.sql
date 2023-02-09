-- start_ignore
-- set the plpython_version
show gp_server_version_num;
\gset
select CASE WHEN :gp_server_version_num < 70000 THEN 'plpythonu' else 'plpython3u' end plpython_version;
\gset
-- end_ignore

CREATE SCHEMA ftsr;
SELECT diskquota.set_schema_quota('ftsr', '1 MB');
SET search_path TO ftsr;
create or replace language :plpython_version;
--
-- pg_ctl:
--   datadir: data directory of process to target with `pg_ctl`
--   command: commands valid for `pg_ctl`
--   command_mode: modes valid for `pg_ctl -m`  
--
create or replace function pg_ctl(gp_server_version int, datadir text, command text, command_mode text default 'immediate')
returns text as $$
    import subprocess
    if command not in ('stop', 'restart'):
        return 'Invalid command input'
    cmd = 'pg_ctl -l postmaster.log -D %s ' % datadir
    cmd = cmd + '-W -m %s %s' % (command_mode, command)
    if (gp_server_version < 70000):
        return subprocess.check_output(cmd, stderr=subprocess.STDOUT, shell=True).replace('.', '')
    else:
        return subprocess.check_output(cmd, stderr=subprocess.STDOUT, shell=True, encoding='utf8').replace('.', '')
$$ language :plpython_version;

create or replace function pg_recoverseg(gp_server_version int, datadir text, command text)
returns text as $$
    import subprocess
    cmd = 'gprecoverseg -%s -d %s; exit 0; ' % (command, datadir)
    if (gp_server_version < 70000):
        return subprocess.check_output(cmd, stderr=subprocess.STDOUT, shell=True).replace('.', '')
    else:
        return subprocess.check_output(cmd, stderr=subprocess.STDOUT, shell=True, encoding='utf8').replace('.', '')
$$ language :plpython_version;

CREATE TABLE a(i int) DISTRIBUTED BY (i);
INSERT INTO a SELECT generate_series(1,100);
INSERT INTO a SELECT generate_series(1,100000);
SELECT diskquota.wait_for_worker_new_epoch();
-- expect insert fail
INSERT INTO a SELECT generate_series(1,100);

-- now one of primary is down
select pg_ctl(:gp_server_version_num, (select datadir from gp_segment_configuration c where c.role='p' and c.content=0), 'stop');

-- switch mirror to primary
select gp_request_fts_probe_scan();

-- check GPDB status
select content, preferred_role, role, status, mode from gp_segment_configuration where content = 0;

-- expect insert fail
INSERT INTO a SELECT generate_series(1,100);

-- increase quota
SELECT diskquota.set_schema_quota('ftsr', '200 MB');

-- pull up failed primary
-- start_ignore
select pg_recoverseg(:gp_server_version_num, (select datadir from gp_segment_configuration c where c.role='p' and c.content=-1), 'a');
select pg_recoverseg(:gp_server_version_num, (select datadir from gp_segment_configuration c where c.role='p' and c.content=-1), 'ar');
select pg_recoverseg(:gp_server_version_num, (select datadir from gp_segment_configuration c where c.role='p' and c.content=-1), 'a');
select pg_recoverseg(:gp_server_version_num, (select datadir from gp_segment_configuration c where c.role='p' and c.content=-1), 'ar');
-- check GPDB status
select content, preferred_role, role, status, mode from gp_segment_configuration where content = 0;
-- end_ignore

SELECT diskquota.wait_for_worker_new_epoch();
SELECT quota_in_mb, nspsize_in_bytes from diskquota.show_fast_schema_quota_view where schema_name='ftsr';
INSERT INTO a SELECT generate_series(1,100);

DROP TABLE a;
DROP SCHEMA ftsr CASCADE;

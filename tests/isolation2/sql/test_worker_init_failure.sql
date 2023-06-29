--
-- Tests for error handling when the worker catches the error during
-- its first run.
--

-- Function checking whether worker on given db is up
CREATE or REPLACE LANGUAGE plpython2u;
CREATE or REPLACE FUNCTION check_worker_presence(dbname text, wait_time int)
  RETURNS boolean
AS $$
    import psutil
    import time
    worker_name = 'bgworker: [diskquota] ' + dbname
    time.sleep(wait_time)
    for proc in psutil.process_iter():
        try:
            if 'postgres' in proc.name().lower():
                for val in proc.cmdline():
                    if worker_name in val:
                        return True
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            pass
    return False
$$ LANGUAGE plpython2u EXECUTE ON MASTER;

-- Test diskquota behavior when an error occurs during the worker's first run.
-- The error leads to process termination. And launcher won't start it again
-- until extension reload or SIGHUP signal.
CREATE EXTENSION diskquota;
SELECT check_worker_presence(current_database(), 0);
SELECT gp_inject_fault('diskquota_worker_initialization', 'error', dbid)
  FROM gp_segment_configuration WHERE role='p' AND content=-1;
SELECT diskquota.init_table_size_table();
SELECT check_worker_presence(current_database(),
  current_setting('diskquota.worker_timeout')::int / 2);
-- Reload configuration and check that worker is up again
!\retcode gpstop -u;
SELECT check_worker_presence(current_database(),
  current_setting('diskquota.worker_timeout')::int / 2);
DROP EXTENSION diskquota;

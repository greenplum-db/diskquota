import subprocess as sp
import os
from typing import List, Dict
import locale

def gp_run(command: List[str]):
    env = os.environ.copy()
    if 'PYTHONPATH' not in env or env['PYTHONPATH'] == '':
        env['PYTHONPATH'] = os.path.join(env['GPHOME'], 'lib', 'python')
    sp.run(command, universal_newlines=True, env=env)

def db_exec(db: str, command: List[str]):
    res = sp.run(['time', 'psql', db], input=command, universal_newlines=True, stdout=sp.PIPE, encoding=locale.getpreferredencoding())
    return res

def db_exec_no_time(db: str, command: List[str]):
    res = sp.run(['psql', '-d', db, '-c', command], universal_newlines=True, stdout=sp.PIPE, encoding=locale.getpreferredencoding())
    return res

def db_clean(db: str):
    db_exec(db, 'DROP EXTENSION IF EXISTS diskquota;')
    gp_run(['dropdb', '--if-exists', db])
    gp_run(['createdb', db])

def db_enable_diskquota(db: str, guc: Dict[str, str]={}):
    gp_run(['gpconfig', '-c', 'shared_preload_libraries', '-v', 'diskquota-2.0.so'])
    gp_run(['gpstop', '-far'])
    for var in guc:
        gp_run('gpconfig', '-c', var, '-v', guc[var])
    gp_run(['gpstop', '-far'])
    db_exec(db, '''
        CREATE EXTENSION diskquota;
        SELECT diskquota.wait_for_worker_new_epoch();
    ''')

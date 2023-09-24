# Monitoring a large number of databases with active tables

from __utils__ import *

def run(db_prefix: str, num_dbs: int, num_tables: int, num_rows_per_table: int, enable_diskquota: bool):
    # +20 to make room for internal background processes and debugging connections
    gp_run(['gpconfig', '-c', 'max_connections', '-v', f'{num_dbs + 20}'])
    gp_run(['gpconfig', '-c', 'max_worker_processes', '-v', f'{num_dbs + 20}'])
    gp_run(['gpstop', '-far'])
    for i in range(num_dbs):
        db_clean(f'{db_prefix}_{i}')
        if enable_diskquota:
            db_enable_diskquota(f'{db_prefix}_{i}')
    for i in range(num_dbs):
        db_exec(f'db_{i}', f'''
            CREATE TABLE t1 (pk int, val int)
            DISTRIBUTED BY (pk)
            PARTITION BY RANGE (pk) (START (1) END ({num_tables}) INCLUSIVE EVERY (1));

            INSERT INTO t1 
            SELECT pk, val
            FROM generate_series(1, {num_rows_per_table}) AS val, generate_series(1, {num_tables}) AS pk;
        ''')

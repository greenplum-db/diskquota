# Writing data to a large number of tables

from __utils__ import *

def run(db: str, num_tables: int, num_rows_per_table: int, enable_diskquota: bool):
    db_clean(db)
    if enable_diskquota:
        db_enable_diskquota(db)
    db_exec(db, f'''
        CREATE TABLE t1 (pk int, val int)
        DISTRIBUTED BY (pk)
        PARTITION BY RANGE (pk) (START (1) END ({num_tables}) INCLUSIVE EVERY (1));
    ''')
    db_exec(db, f'''
        INSERT INTO t1 
        SELECT pk, val
        FROM generate_series(1, {num_rows_per_table}) AS val, generate_series(1, {num_tables}) AS pk;
    ''')

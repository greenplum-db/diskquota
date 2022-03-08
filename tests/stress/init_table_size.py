# Init diskquota.table_size when the number of tables is very large

from __utils__ import *

def run(db: str, num_tables: int, num_rows_per_table: int, enable_diskquota: bool):
    db_clean(db)
    if enable_diskquota:
        db_enable_diskquota(db)

    create_tables_without_partition(db, num_tables, num_rows_per_table)
    db_exec(db, 'SELECT diskquota.init_table_size_table();')

def create_tables_with_partition(db: str, num_tables: int, num_rows_per_table: int):
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

def create_tables_without_partition(db: str, num_tables: int, num_rows_per_table: int):
    for i in range(num_tables):
        db_exec(db, f'''
            CREATE TABLE table_{i} (i int) DISTRIBUTED BY (i);
        ''')
        db_exec(db, f'''
            INSERT INTO table_{i}
            SELECT generate_series(1, {num_rows_per_table});
        ''')

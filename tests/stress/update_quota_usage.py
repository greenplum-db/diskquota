# Update quota usage when the number of possible quota definition is large
# eg. for ((i=1; i<=10; i++)); do PYTHONPATH='' python3 -m tests.stress update_quota_usage --num_tables 2 --num_tablespaces 2 --enable_diskquota 0 --db testdb ; done

from __utils__ import *
from datetime import datetime
import time

def run(db: str, num_tables: int, num_tablespaces: int, enable_diskquota: int):
    print(f'enable_diskquota={enable_diskquota}')

    db_clean(db)
    if enable_diskquota:
        db_enable_diskquota(db)

    db_exec(db, f'''
    create language plpythonu;
    ''')
    db_exec(db, f'''
CREATE or replace FUNCTION mkdir_spc(name text)
RETURNS void
AS $$
  import os
  try:
    if not os.path.exists(name):
      os.mkdir(name)
  except Exception:
    pass
$$ LANGUAGE plpythonu;
    ''')

    Catalog.db = db

    quotas = role_schema_quotas(num_tables, 1e3, enable_diskquota)
    quotas.extend(tablespace_based_quotas(num_tablespaces, 1e3, enable_diskquota))

    if enable_diskquota:
        Catalog.wait()

    time1 = datetime.now()

    # expect insertions succeed
    for q in quotas:
        q.insert_to_table(1e6)

    time2 = datetime.now()
    seconds = (time2 - time1).seconds

    with open(f"result_{enable_diskquota}.txt", 'a') as f:
        f.write(f"{seconds}\n")

    print(len(quotas))

class Catalog:
    db = "testdb"

    def __init__(self):
        self.name = ""
        self.table_name = ""

    def get_name(self):
        return self.name

    def set_tablename(self, table_name):
        self.table_name = f'{self.name}_table_{table_name}'

    def get_tablename(self):
        return self.table_name

    @classmethod
    def exec(cls, stmt):
        if stmt:
            db_exec(Catalog.db, stmt)

    def create_table(self, tablename):
        self.set_tablename(tablename)
        self.exec(self.create_table_stmt())
        self.exec(self.alter_table_stmt())

    def drop_table(self):
        if self.get_tablename():
            self.exec(self.drop_table_stmt())

    def insert_to_table(self, num_rows):
        if not self.get_tablename():
            raise Exception("No table")

        num_rows = int(num_rows)
        self.exec(self.insert_table_stmt(num_rows))

    def create_table_stmt(self):
        pass

    def drop_table_stmt(self):
        pass

    def insert_table_stmt(self, num_rows):
        pass

    def alter_table_stmt(self):
        pass

    @classmethod
    def wait(cls):
        Catalog.exec("SELECT diskquota.wait_for_worker_new_epoch();")

class Role(Catalog):
    idx = 0

    def __init__(self, name):
        super().__init__()
        self.set_name(name)
        stmt = f'CREATE ROLE {self.name};'
        self.exec(stmt)

    def set_name(self, name):
        self.name = f'batch_role_{Role.idx}_{name}'
        Role.idx += 1

    def create_table_stmt(self):
        return f"CREATE TABLE {self.get_tablename()}(i int) DISTRIBUTED BY (i);"

    def drop_table_stmt(self):
        return f"DROP TABLE IF EXISTS {self.get_tablename()};"

    def insert_table_stmt(self, num_rows):
        return f"INSERT INTO {self.get_tablename()} SELECT generate_series(1, {num_rows});"

    def __del__(self):
        self.drop_table()
        self.exec(f"DROP ROLE {self.name};")

    def alter_table_stmt(self):
        return f"ALTER TABLE {self.get_tablename()} OWNER TO {self.name};"

class Schema(Catalog):
    idx = 0

    def __init__(self, name):
        super().__init__()
        self.set_name(name)
        stmt = f'CREATE SCHEMA {self.name};'
        self.exec(stmt)

    def set_name(self, name):
        self.name = f'batch_schema_{Schema.idx}_{name}'
        Schema.idx += 1

    def create_table_stmt(self):
        return f"CREATE TABLE {self.get_full_tablename()}(i int) DISTRIBUTED BY (i);"

    def drop_table_stmt(self):
        return f"DROP TABLE IF EXISTS {self.get_full_tablename()};"

    def insert_table_stmt(self, num_rows):
        return f"INSERT INTO {self.get_full_tablename()} SELECT generate_series(1, {num_rows});"

    def __del__(self):
        self.drop_table()
        self.exec(f"DROP SCHEMA {self.get_name()};")

    def get_full_tablename(self):
        return f"{self.get_name()}.{self.get_tablename()}"

class Tablespace(Catalog):
    idx = 0

    def __init__(self, name):
        super().__init__()
        self.set_name(name)

        dir_name = f'/tmp/tbspc_dir_{self.get_name()}'

        self.exec(f'''
        select mkdir_spc('{dir_name}') from gp_dist_random('gp_id');
        ''')
        self.exec(f'''
        select mkdir_spc('{dir_name}');
        ''')

        stmt = f"CREATE TABLESPACE {self.get_name()} LOCATION '{dir_name}';"
        self.exec(stmt)

    def set_name(self, name):
        self.name = f'batch_tablespace_{Tablespace.idx}_{name}'
        Tablespace.idx += 1

    def __del__(self):
        self.drop_table()
        self.exec(f"DROP TABLESPACE if exists {self.get_name()};")

    def alter_table(self, table_name):
        if not table_name:
            raise Exception("table_name required")

        self.table_name = table_name
        self.exec(f"ALTER TABLE {self.table_name} SET TABLESPACE {self.get_name()};")

    def drop_table_stmt(self):
        return f"DROP TABLE IF EXISTS {self.table_name};"

class Quota:
    def __init__(self, enable_diskquota=True):
        self.enable_diskquota = enable_diskquota

    def __del__(self):
        self.set(-1)

    def put_table(self):
        pass

    def insert_to_table(self, nrows):
        """
        delegated to catalog's method with the same name
        """
        self.get_base_catalog().insert_to_table(nrows)

    def create_table(self, tablename):
        """
        delegated to catalog's method with the same name
        """
        self.get_base_catalog().create_table(tablename)

    def get_base_catalog(self):
        """
        Get role/schema object based on quota type
        """
        pass

    def set_stmt(self, sz):
        pass

    def set(self, sz):
        if self.enable_diskquota:
            self.get_base_catalog().exec(self.set_stmt(sz))

class RoleQuota(Quota):
    def __init__(self, role, sz, enable_diskquota):
        super().__init__(enable_diskquota)
        self.role = role
        self.sz = sz
        self.set(self.sz)

    def get_base_catalog(self):
        return self.role

    def set_stmt(self, sz):
        return f'''
        SELECT diskquota.set_role_quota('{self.role.get_name()}', '{sz} MB')
        '''

class SchemaQuota(Quota):
    def __init__(self, schema, sz, enable_diskquota):
        super().__init__(enable_diskquota)
        self.schema = schema
        self.sz = sz
        self.set(self.sz)

    def get_base_catalog(self):
        return self.schema

    def set_stmt(self, sz):
        return f'''
        SELECT diskquota.set_schema_quota('{self.schema.get_name()}', '{sz} MB');
        '''

class TablespaceSchemaQuota(Quota):
    def __init__(self, schema, spc, sz, enable_diskquota):
        super().__init__(enable_diskquota)
        self.schema = schema
        self.spc = spc
        self.sz = sz
        self.set(sz)

    def get_base_catalog(self):
        return self.schema

    def set_stmt(self, sz):
        return f'''
        SELECT diskquota.set_schema_tablespace_quota('{self.schema.get_name()}', '{self.spc.get_name()}', '{sz} MB');
        '''
        return stmt

    def put_table(self):
        self.spc.alter_table(self.schema.get_full_tablename())

class TablespaceRoleQuota(Quota):
    def __init__(self, role, spc, sz, enable_diskquota):
        super().__init__(enable_diskquota)
        self.role = role
        self.spc = spc
        self.sz = sz
        self.set(sz)

    def get_base_catalog(self):
        return self.role

    def set_stmt(self, sz):
        return f'''
        SELECT diskquota.set_role_tablespace_quota('{self.role.get_name()}', '{self.spc.get_name()}', '{sz} MB');
        '''

    def put_table(self):
        self.spc.alter_table(self.role.get_tablename())

def role_schema_quotas(n, rows, enable_diskquota=True):
    if n < 1:
        raise Exception("invalid n")

    roles, schemas = [], []
    for i in range(n):
        roles.append(Role(i))
        schemas.append(Schema(i))

    quotas = []
    for i in range(n):
        rq = RoleQuota(roles[i], 1, enable_diskquota)
        sq = SchemaQuota(schemas[i], 1, enable_diskquota)

        quotas.append(rq)
        quotas.append(sq)

    for i, q in enumerate(quotas):
        q.create_table(i)
        q.insert_to_table(rows)

    return quotas

def tablespace_based_quotas(n, rows, enable_diskquota=True):
    if n < 1:
        raise Exception(f"invalid n: {n}")

    spcs = [None for _ in range(n * 2)]
    roles, schemas = [], []

    for i in range(n):
        roles.append(Role(i))
        schemas.append(Schema(i))

        spcs[i] = Tablespace(i)
        spcs[i + n] = Tablespace(i + n)

    quotas = []
    for i in range(n):
        trq = TablespaceRoleQuota(roles[i], spcs[i], 1, enable_diskquota)
        tsq = TablespaceSchemaQuota(schemas[i], spcs[i + n], 1, enable_diskquota)

        quotas.append(trq)
        quotas.append(tsq)

    for i, q in enumerate(quotas):
        q.create_table(i)
        q.put_table()
        q.insert_to_table(rows)

    return quotas

# eg. PYTHONPATH='' python3 -m tests.stress rand_quota --enable_diskquota 1 --db testdb

from __utils__ import *
import time
import random
import re
import subprocess
import unittest

QUOTA_TYPE_SCHEMA = 0
QUOTA_TYPE_ROLE = 1
QUOTA_TYPE_SCHEMA_SPC = 2
QUOTA_TYPE_ROLE_SPC = 3


def run(db: str, enable_diskquota: int):
    print(f"enable_diskquota={enable_diskquota}")

    db_clean(db)
    if enable_diskquota:
        db_enable_diskquota(db)
    else:
        gp_run(["gpconfig", "-c", "shared_preload_libraries", "-v", ""])
        gp_run(["gpstop", "-far"])

    lang_plpython(db)
    udf_mkdir(db)
    udf_rmdir(db)

    Catalog.db = db

    state = State()
    state.run()


class Catalog:
    db = "testdb"

    def __init__(self, name):
        if not name:
            raise Exception(f"catalog name: {name} is empty")

        name = str(name)
        # catalog name, eg. role name, schema name, etc
        self.name = self.prefix + name
        # a list of tables belong to this catalog
        self.tables = []
        self.dropped = False

    @classmethod
    def exec(cls, stmt):
        res = None
        if stmt:
            res = db_exec_no_time(Catalog.db, stmt)
            print(f"execute {stmt} on {Catalog.db}")
            res.check_returncode()
        return res

    @classmethod
    def wait(cls):
        Catalog.exec("SELECT diskquota.wait_for_worker_new_epoch();")

    def print_tables(self):
        res = ", ".join([x.get_name() for x in self.tables])
        print(f"tables in {self.get_name()}: [{res}]")

    def put_table_stmt(self, table):
        """
        implemented in subclass
        :return:
        """
        pass

    def drop_stmt(self):
        """
        implemented in subclass
        :return:
        """
        pass

    def set_belong(self, table):
        """
        implemented in subclass
        :param table:
        :return:
        """
        pass

    def _unset_belong(self, table):
        """
        implemented in subclass
        :param table:
        :return:
        """
        pass

    def put_table(self, table):
        """alter table to role/schema/tablespace"""
        self.exec(self.put_table_stmt(table))
        self.tables.append(table)
        self.set_belong(table)

    def remove_table(self, table):
        """
        remove from table list
        """
        self._unset_belong(table)
        self.tables = [t for t in self.tables if t.name != table.name]

    def drop_table(self, table):
        self.remove_table(table)
        table.drop()

    def get_name(self):
        return self.name

    def insert_to_table(self, nrows):
        if not self.tables:
            return

        table = random.choice(self.tables)
        table.insert(nrows)

    def delete_from_table(self, nrows):
        if not self.tables:
            return

        table = random.choice(self.tables)
        table.delete(nrows)

    def drop(self):
        if self.dropped:
            return

        self.dropped = True

        for t in self.tables:
            self.remove_table(t)
            t.drop()
        self.exec(self.drop_stmt())

    def __del__(self):
        self.drop()

    def get_table_size_from_diskquota(self):
        """
        get total size of table_lst from diskquota.table_size
        """
        table_lst = self.tables

        if not table_lst:
            return 0

        table_names = [f"'{x.get_name()}'" for x in table_lst]
        table_lst_str = ",".join(table_names)
        stmt = f"""
SELECT name, size
FROM diskquota.table_size dk JOIN
(SELECT name, name::regclass::oid AS tableid FROM unnest(ARRAY[{table_lst_str}]) AS name) nest
ON nest.tableid = dk.tableid WHERE segid = -1;
"""
        res = Catalog.exec(stmt)
        size_dic = None
        if res:
            size_dic = extract_tables_size(res.stdout)
            return sum(size_dic.values())

        return 0

    def get_total_size_from_diskquota_view(self):
        """
        get size from diskquota view
        """
        stmt = self.get_total_size_from_diskquota_view_stmt()
        if not stmt:
            return 0

        res = self.exec(stmt)
        output = res.stdout
        nums = extract_numbers(output)

        if nums:
            print(f"Total size for ({self.get_name()}): {nums[0]}")
            return nums[0]

        return 0

    def get_total_size_from_diskquota_view_stmt(self):
        pass

    def get_total_size_from_pg(self):
        size_dic = get_tables_size_from_pg(self.tables)
        if size_dic:
            return sum(size_dic.values())

        return 0

    def show_quota_config_stmt(self):
        pass


class Table(Catalog):
    idx = 0

    def __init__(self, name):
        self.prefix = f"table_{Table.idx}_"

        super().__init__(name)

        Table.idx += 1
        self.size = 0
        self.belong_role = None
        self.belong_schema = None
        self.belong_tablespace = None
        self.exec(f"CREATE TABLE {self.name} (i serial, j int) DISTRIBUTED BY (i);")

    def size(self):
        """current table size"""
        return self.size

    @classmethod
    def total_size(cls, lst):
        """get total size of a list of Table"""
        return sum([x.size() for x in lst])

    def insert(self, nrows):
        if not nrows:
            return

        nrows = int(nrows)

        stmt = f"INSERT INTO {self.name}(j) SELECT generate_series(1, {nrows});"
        self.exec(stmt)

    def delete(self, rows_num):
        stmt = f"DELETE FROM {self.name} WHERE i IN (SELECT i FROM {self.name} ORDER BY i LIMIT {rows_num});"
        self.exec(stmt)
        vacuum_stmt = f"VACUUM FULL {self.name};"
        self.exec(stmt)

    def drop_stmt(self):
        return f"DROP TABLE IF EXISTS {self.get_name()};"

    def drop(self):
        if self.dropped:
            return

        self.dropped = True
        for x in [self.belong_role, self.belong_schema, self.belong_tablespace]:
            if x:
                x.remove_table(self)

        self.exec(self.drop_stmt())

    def get_total_size_from_diskquota_view_stmt(self):
        return f"""
SELECT size FROM diskquota.table_size WHERE tableid = '{self.get_name()}'::regclass::oid AND segid = -1;
"""

    def get_total_size_from_pg(self):
        size_dic = get_tables_size_from_pg(self)
        if size_dic:
            return sum(size_dic.values())


class Role(Catalog):
    idx = 0

    def __init__(self, name):
        self.prefix = f"role_{Role.idx}_"

        super().__init__(name)
        Role.idx += 1
        self.exec(f"CREATE ROLE {self.get_name()};")

    def put_table_stmt(self, table):
        return f"ALTER TABLE {table.get_name()} OWNER TO {self.get_name()};"

    def drop_stmt(self):
        return f"DROP ROLE IF EXISTS {self.get_name()};"

    def set_belong(self, table):
        table.belong_role = self

    def _unset_belong(self, table):
        table.belong_role = None

    def get_total_size_from_diskquota_view_stmt(self):
        return f"""
SELECT rolsize_in_bytes FROM diskquota.show_fast_role_quota_view WHERE role_name = '{self.get_name()}';
"""

    def show_quota_config_stmt(self):
        return f"""
SELECT * FROM diskquota.show_fast_role_quota_view WHERE role_name = '{self.get_name()}';
"""


class Schema(Catalog):
    idx = 0

    def __init__(self, name):
        self.prefix = f"schema_{Schema.idx}_"

        super().__init__(name)
        Schema.idx += 1
        self.exec(f"CREATE SCHEMA {self.get_name()};")

    def put_table_stmt(self, table):
        return f"ALTER TABLE {table.get_name()} SET SCHEMA {self.get_name()};"

    def drop_stmt(self):
        return f"DROP SCHEMA IF EXISTS {self.get_name()};"

    def set_belong(self, table):
        table.belong_schema = self

    def _unset_belong(self, table):
        table.belong_schema = None

    def put_table(self, table):
        super().put_table(table)
        # NOTE: name prefix should be stripped after altered to another role/schema
        table.name = f"{self.name}.{table.name}"

    def get_total_size_from_diskquota_view_stmt(self):
        return f"""
SELECT nspsize_in_bytes FROM diskquota.show_fast_schema_quota_view WHERE schema_name = '{self.get_name()}';
"""

    def show_quota_config_stmt(self):
        return f"""
SELECT * FROM diskquota.show_fast_schema_quota_view WHERE schema_name = '{self.get_name()}';
"""


class Tablespace(Catalog):
    idx = 0

    def __init__(self, name):
        self.prefix = f"spc_{Tablespace.idx}_"

        super().__init__(name)
        Tablespace.idx += 1

        self.dir_name = f"/tmp/tbspc_dir_{self.get_name()}"
        self.exec(
            f"""SELECT mkdir_spc('{self.dir_name}') FROM gp_dist_random('gp_id');"""
        )
        self.exec(f"""SELECT mkdir_spc('{self.dir_name}');""")

        stmt = f"CREATE TABLESPACE {self.get_name()} LOCATION '{self.dir_name}';"
        self.exec(stmt)

    def put_table_stmt(self, table):
        return f"ALTER TABLE {table.get_name()} SET TABLESPACE {self.get_name()};"

    def drop_stmt(self):
        return f"DROP TABLESPACE IF EXISTS {self.get_name()};"

    def set_belong(self, table):
        table.belong_tablespace = self

    def _unset_belong(self, table):
        table.belong_tablespace = None

    def drop(self):
        if self.dropped:
            return

        self.dropped = True
        self.exec(self.drop_stmt())
        # print(f'Try to remove dir: {self.dir_name}')
        self.exec(
            f"""SELECT rmdir_spc('{self.dir_name}') FROM gp_dist_random('gp_id');"""
        )
        self.exec(f"""SELECT rmdir_spc('{self.dir_name}');""")


class Quota:
    def __init__(self, limit):
        self.exceed = False
        self.limit = limit
        self.tables = []
        self.set_quota(self.limit)
        self.dropped = False
        self.type = -1

    def __str__(self):
        return f"<{self.name} [{self.get_catalog_str()}]>"

    def get_name(self):
        return self.name

    def get_catalog_str(self):
        """
        implemented in subclass
        :return:
        """
        pass

    def enabled(self):
        return self.limit > 0

    def set_stmt(self, limit):
        """
        implemented in subclass
        :param limit:
        :return:
        """
        pass

    def get_base_catalog(self) -> Catalog:
        """
        implemented in subclass
        :return:
        """
        pass

    def size(self):
        return Table.total_size(self.tables)

    def get_tables(self):
        return self.get_base_catalog().tables

    def get_table_size_from_diskquota(self):
        return self.get_base_catalog().get_table_size_from_diskquota()

    def get_total_size_from_pg(self):
        return self.get_base_catalog().get_total_size_from_pg()

    def in_block_list(self):
        if not self.limit:
            # quota not set
            return False

        sz_bytes = self.get_total_size_from_pg()
        limit_bytes = self.limit * (2**20)
        return sz_bytes >= limit_bytes

    def set_quota(self, limit):
        Catalog.exec(self.set_stmt(limit))

    def delete_quota(self):
        self.set_quota(-1)
        self.limit = 0

    def extend(self, cnt):
        self.limit += cnt
        self.set_quota(self.limit)

    def drop(self):
        if self.dropped:
            return

        self.dropped = True
        self.set_quota(-1)

    def rand_drop_table(self):
        cat = self.get_base_catalog()
        if not cat.tables:
            return

        table = random.choice(cat.tables)
        cat.drop_table(table)

    def insert_to_table(self, rows_num):
        rows_num = int(rows_num)
        self.get_base_catalog().insert_to_table(rows_num)

    def delete_from_table(self, rows_num):
        rows_num = int(rows_num)
        self.get_base_catalog().delete_from_table(rows_num)

    def __del__(self):
        self.drop()

    def show_state(self):
        """
        show current state of quota
        """
        pass

    def show_quota_config_stmt(self):
        return self.get_base_catalog().show_quota_config_stmt()

    def show_quota_config(self):
        stmt = self.show_quota_config_stmt()
        res = Catalog.exec(stmt)
        output = chomp_empty_line(res.stdout)

        print(
            f"""
========== diskquota show_fast_*_quota_view {self.get_name()} ==========
{output}
"""
        )

    def show_table_size_from_diskquota(self):
        """
        show diskquota.table_size
        """

        table_lst = self.get_base_catalog().tables
        if not table_lst:
            return

        table_names = [f"'{x.get_name()}'" for x in table_lst]
        table_lst_str = ",".join(table_names)
        stmt = f"""
SELECT name, size, segid
FROM diskquota.table_size dk JOIN (SELECT name, name::regclass::oid AS tableid FROM unnest(ARRAY[{table_lst_str}]) AS name) nest
ON nest.tableid = dk.tableid ORDER BY segid, name;
"""
        res = Catalog.exec(stmt)
        output = chomp_empty_line(res.stdout)
        print(
            f"""
========== diskquota.table_size {self.get_name()} ==========
{output}
"""
        )

    def show_table_size_from_postgres(self):
        """
        get size of multiple tables from postgres
        """

        table_lst = self.get_base_catalog().tables
        if not table_lst:
            return

        table_names = [f"'{x.get_name()}'" for x in table_lst]
        table_lst_str = ",".join(table_names)
        stmt = f"""
SELECT name AS table_name, pg_total_relation_size(name) AS size FROM unnest(ARRAY[{table_lst_str}]) AS name
ORDER BY table_name;
"""
        res = Catalog.exec(stmt)
        output = chomp_empty_line(res.stdout)
        print(
            f"""
========== pg_total_relation_size {self.get_name()} ==========
{output}
"""
        )

    def show_current_state(self):
        self.show_quota_config()
        self.show_table_size_from_diskquota()
        self.show_table_size_from_postgres()


class SchemaQuota(Quota):
    idx = 0

    def __init__(self, schema, limit):
        self.schema = schema
        SchemaQuota.idx += 1
        self.name = f"SchemaQuota_{SchemaQuota.idx}"

        super().__init__(limit)
        self.type = QUOTA_TYPE_SCHEMA

    def set_stmt(self, sz):
        return f"""
        SELECT diskquota.set_schema_quota('{self.schema.get_name()}', '{sz} MB');
        """

    def get_catalog_str(self):
        return ", ".join([self.schema.get_name()])

    def drop(self):
        super().drop()
        self.schema.drop()

    def put_table(self, t):
        self.schema.put_table(t)

    def get_base_catalog(self):
        return self.schema


class RoleQuota(Quota):
    idx = 0

    def __init__(self, role, limit):
        self.role = role
        RoleQuota.idx += 1
        self.name = f"RoleQuota_{RoleQuota.idx}"

        super().__init__(limit)
        self.type = QUOTA_TYPE_ROLE

    def get_base_catalog(self):
        return self.role

    def set_stmt(self, sz):
        return f"""
SELECT diskquota.set_role_quota('{self.role.get_name()}', '{sz} MB');"""

    def get_catalog_str(self):
        return ", ".join([self.role.get_name()])

    def drop(self):
        super().drop()
        self.role.drop()

    def put_table(self, t):
        self.role.put_table(t)


class TablespaceSchemaQuota(Quota):
    idx = 0

    def __init__(self, tablespace, schema, limit):
        self.tablespace = tablespace
        self.schema = schema
        TablespaceSchemaQuota.idx += 1
        self.name = f"TablespaceSchemaQuota_{TablespaceSchemaQuota.idx}"

        super().__init__(limit)
        self.type = QUOTA_TYPE_SCHEMA_SPC

    def set_stmt(self, sz):
        return f"""
SELECT diskquota.set_schema_tablespace_quota('{self.schema.get_name()}', '{self.tablespace.get_name()}', '{sz} MB')
"""

    def drop(self):
        super().drop()
        self.schema.drop()
        self.tablespace.drop()

    def put_table(self, t):
        self.schema.put_table(t)
        self.tablespace.put_table(t)

    def get_catalog_str(self):
        return ", ".join([self.schema.get_name(), self.tablespace.get_name()])

    def get_base_catalog(self):
        return self.schema

    def show_quota_config_stmt(self):
        return f"""
SELECT * FROM diskquota.show_fast_schema_tablespace_quota_view
WHERE schema_name = '{self.schema.get_name()}' AND tablespace_name = '{self.tablespace.get_name()}';
"""


class TablespaceRoleQuota(Quota):
    idx = 0

    def __init__(self, tablespace, role, limit):
        self.tablespace = tablespace
        self.role = role
        TablespaceRoleQuota.idx += 1
        self.name = f"TablespaceRoleQuota_{TablespaceRoleQuota.idx}"

        super().__init__(limit)
        self.type = QUOTA_TYPE_ROLE_SPC

    def get_base_catalog(self):
        return self.role

    def get_catalog_str(self):
        return ", ".join([self.role.get_name(), self.tablespace.get_name()])

    def set_stmt(self, sz):
        return f"""
SELECT diskquota.set_role_tablespace_quota('{self.role.get_name()}', '{self.tablespace.get_name()}', '{sz} MB');
"""

    def drop(self):
        super().drop()
        self.role.drop()
        self.tablespace.drop()

    def put_table(self, t):
        self.role.put_table(t)
        self.tablespace.put_table(t)

    def show_quota_config_stmt(self):
        return f"""
SELECT * FROM diskquota.show_fast_role_tablespace_quota_view
WHERE role_name = '{self.role.get_name()}' AND tablespace_name = '{self.tablespace.get_name()}';
"""


class State:
    def __init__(self):
        self.quotas = []
        self.actions = []

    def __str__(self):
        y = ", ".join([x.__str__() for x in self.quotas])
        return f"quotas: {len(self.quotas)} ({y})"

    def run(self):
        cnt = 0

        try:
            while True:
                self.get_new_action(cnt)
                print(self)

                # sleep
                time.sleep(1)
                # next loop
                cnt += 1
                print(f"loop cnt: {cnt}")

                Catalog.wait()

                self.check_table_size()
                continue
        except KeyboardInterrupt:
            return

    def rand_new_quota(self, cnt):
        x = random.randint(0, 3)
        pname = str(cnt)
        t = Table(pname)

        if x == QUOTA_TYPE_ROLE:
            r = Role(pname)
            r.put_table(t)
            quota = RoleQuota(r, 1)
        elif x == QUOTA_TYPE_SCHEMA:
            s = Schema(pname)
            s.put_table(t)
            quota = SchemaQuota(s, 1)
        elif x == QUOTA_TYPE_ROLE_SPC:
            spc = Tablespace(pname)
            r = Role(pname)
            quota = TablespaceRoleQuota(spc, r, 1)
        else:
            spc = Tablespace(pname)
            s = Schema(pname)
            quota = TablespaceSchemaQuota(spc, s, 1)

        self.quotas.append(quota)

        return quota

    def check_table_size(self):
        # check table size in quota config
        for q in self.quotas:
            if not q.enabled():
                continue

            size_diskquota = q.get_table_size_from_diskquota()
            size_pg = q.get_total_size_from_pg()

            print(
                f"""
quota: {q}, size_diskquota: {size_diskquota}, size_pg: {size_pg}
"""
            )
            q.show_current_state()
            if size_diskquota != size_pg:
                raise Exception(
                    f"table_size in diskquota != postgres. size_diskquota: {size_diskquota}, size_pg: {size_pg}"
                )

    def get_new_action(self, cnt):
        actions_map = {
            "new_quota": self.rand_new_quota,  # create quota
            "delete_quota": self.rand_delete_quota,  # set quota to -1
            "erase_quota": self.rand_erase_quota,  # set quota to -1, delete related objects
            "new_table": self.rand_new_table,  # create table, put it to quota
            "insert_to_table": self.rand_insert_to_table,
            "delete_from_table": self.rand_delete_from_table,  # delete from table and vacuum full
            "drop_table": self.rand_drop_table,
            "extend_quota": self.rand_extend_quota,  # extend quota by 1MB
        }

        if self.quotas:
            action = random.choice(list(actions_map.keys()))
        else:
            action = "new_quota"

        print(f"action={action}")

        func = actions_map[action]
        func(str(cnt))

    def rand_delete_quota(self, _):
        if self.quotas:
            quota = random.choice(self.quotas)
            quota.delete_quota()

    def rand_erase_quota(self, _):
        if self.quotas:
            quota = random.choice(self.quotas)
            quota.drop()
            self.quotas = [x for x in self.quotas if x.idx != quota.idx]

    def rand_new_table(self, cnt):
        if not self.quotas:
            return

        quota = random.choice(self.quotas)
        t = Table(cnt)
        quota.put_table(t)

    def rand_insert_to_table(self, _):
        if not self.quotas:
            return

        quota = random.choice(self.quotas)
        self.try_run(quota, lambda x: x.insert_to_table(1e4))

    def rand_delete_from_table(self, _):
        if not self.quotas:
            return

        quota = random.choice(self.quotas)
        quota.delete_from_table(1e4)

    def try_run(self, quota, func):
        blocked = quota.in_block_list()

        try:
            func(quota)
        except subprocess.CalledProcessError as err:
            if blocked:
                print(f"Insertion for {quota} exceeded as expected: {err}")
            else:
                quota.show_current_state()
                raise Exception(f"Insertion for {quota} should succeed: {err}")
        else:
            if blocked:
                quota.show_current_state()
                raise Exception(f"Insertion for {quota} should have failed")
            else:
                print(f"Insertion for {quota} succeeded as expected")

    def rand_drop_table(self, _):
        if not self.quotas:
            return

        quota = random.choice(self.quotas)
        quota.rand_drop_table()

    def rand_extend_quota(self, _):
        if not self.quotas:
            return

        quota = random.choice(self.quotas)
        quota.extend(1)


def lang_plpython(db):
    db_exec(db, "CREATE LANGUAGE plpythonu;")


def udf_mkdir(db):
    db_exec(
        db,
        f"""
CREATE OR REPLACE FUNCTION mkdir_spc(name text)
RETURNS void
AS $$
  import os
  try:
    if not os.path.exists(name):
      os.mkdir(name)
  except Exception:
    pass
$$ LANGUAGE plpythonu;
""",
    )


def udf_rmdir(db):
    db_exec(
        db,
        f"""
CREATE OR REPLACE FUNCTION rmdir_spc(name text)
RETURNS void
AS $$
  import os
  try:
    if os.path.exists(name):
      os.rmdir(name)
  except Exception:
    pass
$$ LANGUAGE plpythonu;
""",
    )


def extract_numbers(txt):
    """
    extract numbers from text
    """
    lst = txt.split("\n")
    res = []
    for x in lst:
        x = x.strip()
        if re.match(r"\d+", x):
            res.append(int(x))
    return res


def extract_tables_size(txt):
    """
    extract multiple table's size from text
    eg.
     i  | pg_total_relation_size
    ----+------------------------
     t  |                1277952
     t2 |                1769472
    (2 rows)
    """
    lst = txt.split("\n")
    res = {}

    for line in lst:
        if is_size_line(line):
            lst = line.split()
            lst = [x.strip() for x in lst]
            table_name = lst[0]
            table_size = int(lst[-1])
            res[table_name] = table_size

    return res


def is_size_line(line):
    """
    Check whether the current line contains table size
    """
    lst = line.split(" ")
    lst = [x.strip() for x in lst]
    if lst:
        item = lst[-1]
        return re.match(r"\d+", item)

    return False


def get_table_size_from_pg(table):
    tablename = table.get_name()
    res = Catalog.exec(f"""SELECT pg_total_relation_size('{tablename}')""")
    sz = 0
    if res:
        sz = extract_numbers(res.stdout)

    return sz


def get_tables_size_from_pg(table_lst):
    """
    get size of multiple tables from postgres
    """
    if not table_lst:
        return

    table_names = [f"'{x.get_name()}'" for x in table_lst]
    table_lst_str = ",".join(table_names)
    stmt = f"""
SELECT name as table_name, pg_total_relation_size(name) as size from unnest(ARRAY[{table_lst_str}]) as name;
"""
    res = Catalog.exec(stmt)
    size_dic = None
    if res:
        size_dic = extract_tables_size(res.stdout)

    return size_dic


def chomp_empty_line(txt):
    lst = txt.split("\n")
    lst = [x for x in lst if x != "\n" and x]
    output = "\n".join(lst)
    return output


class TestCatalog(unittest.TestCase):
    def test_role(self):
        t = Table("t1")
        r = Role("r1")
        r.put_table(t)

        t.insert(10000)

        print(r.get_total_size_from_diskquota_view())
        t.drop()

    def test_table(self):
        t = Table("1")
        t1 = Table(1)
        t2 = Table(1)

        t.insert(10000)
        t1.insert(10000)
        t2.insert(20000)

        get_tables_size_from_pg([t, t1, t2])

    def test_schema(self):
        t = Table("t2")
        s = Schema("s1")
        s.put_table(t)

        s.drop()

    def test_tablespace(self):
        t = Table("t3")
        spc = Tablespace("spc1")
        spc.put_table(t)

    def test_set_belong(self):
        r = Role("r1")
        t1 = Table("t1")
        r.put_table(t1)

        print(t1.belong_role.get_name())
        t2 = Table("t2")
        r.put_table(t2)

        for t in r.tables:
            print(t.name)

        t2.drop()

        for x in r.tables:
            print(x.get_name())


class TestQuota(unittest.TestCase):
    def test_role_quota(self):
        t = Table("t4")
        role = Role("r4")
        role.put_table(t)
        rq = RoleQuota(role, 1)

    def test_schema_quota(self):
        t = Table("t5")
        schema = Schema("s5")
        schema.put_table(t)
        SchemaQuota(schema, 1)

    def test_tablespace_role_quota(self):
        t = Table("t6")
        role = Role("t6")
        role.put_table(t)
        spc = Tablespace("spc6")
        TablespaceRoleQuota(spc, role, 1)

    def test_tablespace_schema_quota(self):
        t = Table("t7")
        schema = Schema("s7")
        schema.put_table(t)
        spc = Tablespace("spc7")
        TablespaceSchemaQuota(spc, schema, 1)

    def test_quota_exceed(self):
        r = Role(1)

        quota = RoleQuota(r, 1)

        for i in range(2):
            t = Table(i + 1)
            Catalog.wait()
            t.insert(10000)
            r.put_table(t)

        Catalog.wait()
        try:
            quota.insert_to_table(1e4)
            Catalog.wait()
            quota.insert_to_table(1e4)
        except subprocess.CalledProcessError as err:
            print(f"Failed as expected: {err}")
        else:
            raise Exception(f"Insertion should have failed for quota: {quota}")

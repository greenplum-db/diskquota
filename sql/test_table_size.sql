-- Test tablesize table

create table a(i text);

insert into a select * from generate_series(1,10000);

select pg_sleep(2);
create table buffer(oid oid, relname name, size bigint);

with size as ( select oid,relname,pg_total_relation_size(oid) from pg_class) insert into buffer select size.oid, size.relname, size.pg_total_relation_size  from size, diskquota.table_size as dt where dt.tableid = size.oid and relname = 'a';

insert into buffer select oid, relname, sum(pg_total_relation_size(oid)) from gp_dist_random('pg_class') where oid > 16384 and (relkind='r' or relkind='m') and relname = 'a' group by oid, relname;

select sum(buffer.size) = diskquota.table_size.size from buffer, diskquota.table_size where buffer.oid = diskquota.table_size.tableid group by diskquota.table_size.size;

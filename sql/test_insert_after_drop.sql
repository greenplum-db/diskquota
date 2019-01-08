create database db_insert_after_drop;
\c db_insert_after_drop
create extension diskquota;
-- Test Drop Extension
create schema sdrtbl;
select diskquota.set_schema_quota('sdrtbl', '1 MB');
set search_path to sdrtbl;
create table a(i int);
insert into a select generate_series(1,100);
-- expect insert fail
insert into a select generate_series(1,100000000);
select pg_sleep(5);
insert into a select generate_series(1,100);
drop extension diskquota;
-- no sleep, it will take effect immediately
insert into a select generate_series(1,100);

drop table a;
\c postgres
drop database db_insert_after_drop;

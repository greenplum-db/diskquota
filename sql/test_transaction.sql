-- Test schema
create schema ts1;
select diskquota.set_schema_quota('ts1', '1 MB');
BEGIN;
create table ts1.a(i int);
-- expect insert succeed
insert into ts1.a select generate_series(1,100);
-- expect insert fail
insert into ts1.a select generate_series(1,100000000);
END;

SELECT pg_sleep(5);

BEGIN;
create table ts1.a(i int);
-- expect insert succeed
insert into ts1.a select generate_series(1,100);
create table ts1.a2(i int);
-- expect insert succeed
insert into ts1.a2 select generate_series(1,100);
END;

-- expect insert succeed
insert into ts1.a2 select generate_series(1,100);
-- expect insert fail
insert into ts1.a select generate_series(1,100000000);

BEGIN;
-- expect insert fail
insert into ts1.a2 select generate_series(1,100);
END;


BEGIN;
drop table ts1.a;
ABORT;

select pg_sleep(5);

BEGIN;
-- expect insert fail
insert into ts1.a2 select generate_series(1,100);
END;

BEGIN;
drop table ts1.a;
END;

select pg_sleep(5);

BEGIN;
-- expect insert succeed
insert into ts1.a2 select generate_series(1,100);
END;

BEGIN;
create table ts1.a(i int);
END;

-- expect insert fail
insert into ts1.a select generate_series(1,100000000);

BEGIN;
truncate table ts1.a;
truncate table ts1.a2;
savepoint p1;
drop table ts1.a;
rollback to p1;
END;

select pg_sleep(5);

BEGIN;
-- expect insert succeed
insert into ts1.a select generate_series(1,100);
-- expect insert fail
insert into ts1.a select generate_series(1,100000000);
END;

drop schema ts1 CASCADE;


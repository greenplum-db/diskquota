CREATE FUNCTION diskquota.test_send_message(IN int4, IN int4, OUT a int4, OUT b int4) RETURNS SETOF RECORD STRICT AS '$libdir/diskquota-3.0.so' LANGUAGE C;

SELECT gp_inject_fault_infinite('diskquota_message_send', 'suspend', dbid)
  FROM gp_segment_configuration WHERE role='p' AND content=-1;

1&: SELECT diskquota.test_send_message(1, 2);
2&: SELECT diskquota.test_send_message(3, 4);

SELECT gp_wait_until_triggered_fault('diskquota_message_send', 2, dbid)
  FROM gp_segment_configuration WHERE role='p' AND content=-1;

SELECT gp_inject_fault_infinite('diskquota_message_send', 'reset', dbid)
  FROM gp_segment_configuration WHERE role='p' AND content=-1;

1<:
2<:

DROP FUNCTION diskquota.test_send_message(int4, int4);

CREATE EXTENSION diskquota;

-- TODO: apply diskquota.init_table_size_table()
-- SELECT diskquota.init_table_size_table();

-- TODO: wait for apply diskquota.wait_for_worker_new_epoch()
-- Wait after init so that diskquota.state is clean
-- SELECT diskquota.wait_for_worker_new_epoch();
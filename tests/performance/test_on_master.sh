#!/bin/bash

set -e

db_name=$1
report_path=$2
test_name=$3

TEST_TIME=1200
JOBS=32
SCRIPT_PATH=$(realpath "${BASH_SOURCE[0]}")
SCRIPT_DIR=$(dirname "${SCRIPT_PATH}")

call_pgbench() {
    pgbench -n -T $TEST_TIME -c 1 -j 1 -f $1 ${db_name} >> ${report_path}
}

call_pgbench_multi_jobs() {
    pgbench -n -T $TEST_TIME -c "${JOBS}" -j "${JOBS}" -f $1 ${db_name} >> ${report_path}
}

clean_buffer_cache() {
    gpssh -f gp_hosts -- sudo sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
}

usage() {
    echo "./test_on_master.sh <db_name> <report_path> [test_name]"
}

insert() {
    psql -f create.sql "${db_name}"
    echo "insert" | tee -a "${report_path}"
    clean_buffer_cache
    call_pgbench "insert.sql"
}

insert_multi_jobs() {
    psql -f create.sql ${db_name}
    echo "insert_multi_jobs" | tee -a "${report_path}"
    clean_buffer_cache
    call_pgbench_multi_jobs "insert.sql"
}

insert_partioned() {
    psql -f create_partitioned.sql ${db_name}
    echo "insert_partioned" | tee -a "${report_path}"
    clean_buffer_cache
    call_pgbench "insert.sql"
}

insert_multi_jobs_partioned() {
    psql -f create_partitioned.sql ${db_name}
    echo "insert_multi_jobs_partioned" | tee -a "${report_path}"
    clean_buffer_cache
    call_pgbench_multi_jobs "insert.sql"
}

create_table_as() {
    # This test requires t1 to exist first
    echo "create_table_as" | tee -a "${report_path}"
    clean_buffer_cache
    call_pgbench "create_table_as.sql"
}

copy_on_segment_multi_jobs() {
    echo "COPY t3 FROM '${SCRIPT_DIR}/copy_seg<SEGID>.csv' ON SEGMENT CSV;" > "${SCRIPT_DIR}/copy_on_seg.sql"
    psql -f create_copy.sql ${db_name}
    echo "copy_on_segment_multi_jobs" | tee -a "${report_path}"
    clean_buffer_cache
    call_pgbench_multi_jobs "copy_on_seg.sql"
}

insert_many_table_small() {
    for i in {1..1000}; do
        psql -c "CREATE TABLE small_t_${i} (id SERIAL, content text) DISTRIBUTED BY (id)" ${db_name}
    done
    echo "insert_many_table_small" | tee -a ${report_path}
    clean_buffer_cache
    call_pgbench_multi_jobs "insert_many_table_small.sql"
}

if [ -z "${db_name}" ] || [ -z "${report_path}" ]; then
    usage
    exit 1
fi

rm -f "${report_path}"

if [ -z "${test_name}" ]; then
    insert
    echo "" >> "${report_path}"
    insert_multi_jobs
    echo "" >> "${report_path}"
    insert_partioned
    echo "" >> "${report_path}"
    insert_multi_jobs_partioned
    echo "" >> "${report_path}"
    create_table_as
    echo "" >> "${report_path}"
    copy_on_segment_multi_jobs
    echo "" >> "${report_path}"
    insert_many_table_small
else
    $test_name
fi

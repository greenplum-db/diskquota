#!/bin/bash

set -e

db_name=$1
report_path=$2
test_name=$3

TEST_TIME=600
JOBS=32
SCRIPT_PATH=$(realpath "${BASH_SOURCE[0]}")
SCRIPT_DIR=$(dirname "${SCRIPT_PATH}")

usage() {
    echo "./test.sh <db_name> <report_file> [test_name]"
}

insert() {
    psql -f create.sql "${db_name}"
    echo "insert" | tee -a "${report_path}"
    gpssh -f gp_hosts -- sudo sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
    pgbench -n -T $TEST_TIME -c 1 -j 1 -f insert.sql "${db_name}" >> "${report_path}"
}

insert_multi_jobs() {
    psql -f create.sql "${db_name}"
    echo "insert_multi_jobs" | tee -a "${report_path}"
    gpssh -f gp_hosts -- sudo sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
    pgbench -n -T $TEST_TIME -c "${JOBS}" -j "${JOBS}" -f insert.sql "${db_name}" >> "${report_path}"
}

insert_partioned() {
    psql -f create_partitioned.sql "${db_name}"
    echo "insert_partioned" | tee -a "${report_path}"
    gpssh -f gp_hosts -- sudo sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
    pgbench -n -T $TEST_TIME -c 1 -j 1 -f insert.sql "${db_name}" >> "${report_path}"
}

insert_multi_jobs_partioned() {
    psql -f create_partitioned.sql "${db_name}"
    echo "insert_multi_jobs_partioned" | tee -a "${report_path}"
    gpssh -f gp_hosts -- sudo sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
    pgbench -n -T $TEST_TIME -c "${JOBS}" -j "${JOBS}" -f insert.sql "${db_name}" >> "${report_path}"
}

create_table_as() {
    # This test requires t1 to exist first
    echo "create_table_as" | tee -a "${report_path}"
    gpssh -f gp_hosts -- sudo sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
    pgbench -n -T $TEST_TIME -c 1 -j 1 -f create_table_as.sql "${db_name}" >> "${report_path}"
}

copy_on_segment_multi_jobs() {
    echo "COPY t3 FROM '${SCRIPT_DIR}/copy_seg<SEGID>.csv' ON SEGMENT CSV;" > "${SCRIPT_DIR}/copy_on_seg.sql"
    psql -f create_copy.sql "${db_name}"
    echo "copy_on_segment_multi_jobs" | tee -a "${report_path}"
    gpssh -f gp_hosts -- sudo sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
    pgbench -n -T $TEST_TIME -c "${JOBS}" -j "${JOBS}" -f copy_on_seg.sql "${db_name}" >> "${report_path}"
}

insert_many_table_small() {
    for i in {1..1000}; do
        psql -c "CREATE TABLE small_t_${i} (id SERIAL, content text) DISTRIBUTED BY (id)" "${db_name}"
    done
    echo "insert_many_table_small" | tee -a "${report_path}"
    gpssh -f gp_hosts -- sudo sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
    pgbench -n -T $TEST_TIME -c "${JOBS}" -j "${JOBS}" -f insert_many_table_small.sql "${db_name}" >> "${report_path}"
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

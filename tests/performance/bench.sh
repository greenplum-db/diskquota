host_ip=$1
loop=$2
db_name=$3
prefix=$4

HOST_NAME="gpadmin@${host_ip}"
PORT=5432

tasks=("insert" "insert_multi_jobs" "insert_partioned" "insert_multi_jobs_partioned")
rm -rf ${prefix}
mkdir -p ${prefix}

# disable diskquota
ssh ${HOST_NAME} "gpconfig -c shared_preload_libraries -v ''"
ssh ${HOST_NAME} "gpstop -ari"

for task in ${tasks[@]}
do
    for (( i = 0 ; i < $loop ; i++ ))
    do
        dropdb -h ${host_ip} -p ${PORT} -U gpadmin ${db_name}
        createdb -h ${host_ip} -p ${PORT} -U gpadmin ${db_name}
        ssh ${HOST_NAME} "gpstop -ari"
        ./test.sh ${host_ip} ${db_name} ${prefix}/disable_${task}_${i} ${task}
    done
done

ssh ${HOST_NAME} "gpconfig -c shared_preload_libraries -v 'diskquota-2.2'"
ssh ${HOST_NAME} "gpstop -ari"
ssh ${HOST_NAME} "gpconfig -c diskquota.hard_limit -v 'on'"
ssh ${HOST_NAME} "gpstop -ari"

# diskquota with hardlimit
for task in ${tasks[@]}
do
    for (( i = 0 ; i < $loop ; i++ ))
    do
        psql -h ${host_ip} -p ${PORT} -U gpadmin -c "DROP EXTENSION diskquota" ${db_name}
        dropdb -h ${host_ip} -p ${PORT} -U gpadmin ${db_name}
        createdb -h ${host_ip} -p ${PORT} -U gpadmin ${db_name}
        psql -h ${host_ip} -p ${PORT} -U gpadmin -c "CREATE EXTENSION diskquota" ${db_name}
        psql -h ${host_ip} -p ${PORT} -U gpadmin -c "SELECT diskquota.set_schema_quota('public', '1000 GB')" ${db_name}
        ssh ${HOST_NAME} "gpstop -ari"
        ./test.sh ${host_ip} ${db_name} ${prefix}/hardlimit_${task}_${i} ${task}
        psql -h ${host_ip} -p ${PORT} -U gpadmin -c "select * from diskquota.show_diskquota_latency;" ${db_name} >> ${prefix}/hardlimit_${task}_${i}
        psql -h ${host_ip} -p ${PORT} -U gpadmin -c "select insert_count, delete_count from diskquota.show_latency();" ${db_name} >> ${prefix}/hardlimit_${task}_${i}
    done
done

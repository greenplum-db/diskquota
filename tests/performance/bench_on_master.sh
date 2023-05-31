loop=$1
db_name=$2
prefix=$3

tasks=("insert" "insert_multi_jobs" "insert_partioned" "insert_multi_jobs_partioned" "copy_on_segment_multi_jobs")
rm -rf ${prefix}
mkdir -p ${prefix}

gpconfig -c log_statement -v 'none'

# disable diskquota
gpconfig -c shared_preload_libraries -v '' --verbose
gpstop -ari

for task in ${tasks[@]}
do
    for (( i = 0 ; i < $loop ; i++ ))
    do
        dropdb ${db_name}
        createdb ${db_name}
        gpstop -ari
        if [[ $? != 0 ]]
        then
            echo 'ERRER: gpstop -ari failed.'
            exit
        fi

        ./test_on_master.sh ${db_name} ${prefix}/disable_${task}_${i} ${task}
    done
done

createdb diskquota
gpconfig -c shared_preload_libraries -v 'diskquota-2.2' --verbose
gpstop -ari
gpconfig -c diskquota.hard_limit -v 'on' --verbose
gpstop -ari

# diskquota with hardlimit
for task in ${tasks[@]}
do
    for (( i = 0 ; i < $loop ; i++ ))
    do
        psql -c "DROP EXTENSION diskquota" ${db_name}
        dropdb ${db_name}
        createdb ${db_name}
        psql -c "CREATE EXTENSION diskquota" ${db_name}
        psql -c "SELECT diskquota.set_schema_quota('public', '10000 GB')" ${db_name}
        gpstop -ari
        if [[ $? != 0 ]]
        then
            echo 'ERRER: gpstop -ari failed.'
            exit
        fi

        ./test_on_master.sh ${db_name} ${prefix}/hardlimit_${task}_${i} ${task}
        psql -c "select * from diskquota.show_diskquota_latency;" ${db_name} >> ${prefix}/hardlimit_${task}_${i}
        psql -c "select insert_count, delete_count from diskquota.show_latency();" ${db_name} >> ${prefix}/hardlimit_${task}_${i}
    done
done

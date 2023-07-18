# fast update diskquota
```
cd diskquota/build
git fetch origin
git pull -r origin test/stress
make install
```

# Setup
## on client
When the pgbench is run on the master, the step should be executed on the master.
### set the `gp_hosts`, for example:
```
hz-diskquota-performance-00
hz-diskquota-performance-01
hz-diskquota-performance-02
```

## on segments
### prepare copy_seg.csv
```
segment_number=12
for (( i = 0 ; i < $segment_number ; i ++ ))
do
cp copy.csv copy_seg$i.csv
done
```

# Experiment
## Execute bench.sh and generate report
```
./bench.sh master_ip times_of_tests testdb prefix
./generate_report.py prefix > report.csv
```

## Execute bench_on_master.sh and generate report
```
./bench_on_master.sh times_of_tests testdb prefix
./generate_report.py prefix > report.csv
```

## To execute the whole test once:
Execute test:
```
dropdb testdb
createdb testdb
./test.sh testdb diskquota_disabled.txt

gpconfig -c shared_preload_libraries -v 'diskquota-2.2'
gpstop -ari
gpconfig -c diskquota.hard_limit -v 'on'
gpstop -ari
psql -c "DROP EXTENSION diskquota" testdb
dropdb testdb
createdb testdb
psql -c "CREATE EXTENSION diskquota" testdb
psql -c "SELECT diskquota.set_schema_quota('public', '1000 GB')" testdb
./test.sh testdb diskquota_enabled.txt
```

To generate the report:
```
./make_report.py diskquota_disabled.txt diskquota_enabled.txt > report.csv
```

psql -d test_ml -c "DROP EXTENSION diskquota;"
dropdb test_ml
createdb test_ml

psql -d test_ml -c "CREATE EXTENSION diskquota;"

psql -d test_ml -c "SELECT diskquota.test_send_message_loop(1);" &
psql -d test_ml -c "SELECT diskquota.test_send_message_loop(1000);" &

for (( ; ; ))
do
    gpstop -u
    sleep 0.1s
done
\! gpconfig -c shared_preload_libraries -v 'diskquota.so' > /dev/null
\! gpstop -raf > /dev/null

\! gpconfig -s 'shared_preload_libraries'

\c
alter extension diskquota update to '1.0';
-- 2.0 to 1.0 need reboot
\! gpstop -arf > /dev/null

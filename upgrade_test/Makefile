REGRESS = dummy

REGRESS_OPTS = --schedule=schedule_1.0--2.0 \
			   --schedule=schedule_2.0--1.0 \
			   --schedule=schedule_misc \
			   --init-file=init_file

PGXS := $(shell pg_config --pgxs)
include $(PGXS)

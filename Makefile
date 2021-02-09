# contrib/diskquota/Makefile

MODULE_big = diskquota

EXTENSION = diskquota
EXTENSION_VERSION = 1.0.3

DATA = \
		diskquota--1.0.sql \
		diskquota--1.0--1.0.3.sql \

DATA_built = diskquota--$(EXTENSION_VERSION).sql

SRCDIR = ./
FILES = diskquota.c enforcement.c quotamodel.c gp_activetable.c diskquota_utility.c
OBJS = diskquota.o enforcement.o quotamodel.o gp_activetable.o diskquota_utility.o
PG_CPPFLAGS = -I$(libpq_srcdir)
SHLIB_LINK = $(libpq)

REGRESS = dummy
ifeq ("$(INTEGRATION_TEST)","y")
REGRESS_OPTS = --schedule=diskquota_schedule_int --init-file=init_file
else
REGRESS_OPTS = --schedule=diskquota_schedule --init-file=init_file
endif
PGXS := $(shell pg_config --pgxs)
include $(PGXS)

diskquota--$(EXTENSION_VERSION).sql:
	cat $(DATA) > diskquota--$(EXTENSION_VERSION).sql

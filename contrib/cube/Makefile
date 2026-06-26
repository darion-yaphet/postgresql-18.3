# contrib/cube/Makefile

MODULE_big = cube
OBJS = \
	$(WIN32RES) \
	cube.o \
	cubeparse.o \
	cubescan.o

EXTENSION = cube
DATA = cube--1.2.sql cube--1.2--1.3.sql cube--1.3--1.4.sql cube--1.4--1.5.sql \
	cube--1.1--1.2.sql cube--1.0--1.1.sql
PGFILEDESC = "cube - multidimensional cube data type"

HEADERS = cubedata.h

REGRESS = cube cube_sci

SHLIB_LINK += $(filter -lm, $(LIBS))

EXTRA_CLEAN = cubeparse.h cubeparse.c cubescan.c

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/cube
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# See notes in src/backend/parser/Makefile about the following two rules
#
# 请参阅 src/backend/parser/Makefile 中关于以下两条规则的注释
cubeparse.h: cubeparse.c
	touch $@

cubeparse.c: BISONFLAGS += -d

# Force these dependencies to be known even without dependency info built:
#
# 即使没有构建依赖信息，也强制了解这些依赖关系：
cubeparse.o cubescan.o: cubeparse.h

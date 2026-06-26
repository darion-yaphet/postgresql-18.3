# contrib/passwordcheck/Makefile

MODULE_big = passwordcheck
OBJS = \
	$(WIN32RES) \
	passwordcheck.o
PGFILEDESC = "passwordcheck - strengthen user password checks"

# uncomment the following two lines to enable cracklib support
#
# 取消注释以下两行以启用racklib支持
# PG_CPPFLAGS = -DUSE_CRACKLIB '-DCRACKLIB_DICTPATH="/usr/lib/cracklib_dict"'
#
# PG_CPPFLAGS = -DUSE_CRACKLIB '-DCRACKLIB_DICTPATH="/usr/lib/cracklib_dict"'
#
# PG_CPPFLAGS = -DUSE_CRACKLIB '-DCRACKLIB_DICTPATH="/usr/lib/cracklib_dict"'
#
# PG_CPPFLAGS = -DUSE_CRACKLIB '-DCRACKLIB_DICTPATH="/usr/lib/cracklib_dict"'
# SHLIB_LINK = -lcrack
#
# SHLIB_LINK = -lcrack
#
# SHLIB_LINK = -lcrack
#
# SHLIB_LINK = -lcrack

REGRESS = passwordcheck

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/passwordcheck
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

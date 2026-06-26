# contrib/basic_archive/Makefile

MODULES = basic_archive
PGFILEDESC = "basic_archive - basic archive module"

REGRESS = basic_archive
REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/basic_archive/basic_archive.conf

# Disabled because these tests require special configuration, which
#
# 禁用，因为这些测试需要特殊配置，这
# typical installcheck users do not have (e.g. buildfarm clients).
#
# 典型的 installcheck 用户没有（例如 buildfarm 客户端）。
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/basic_archive
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

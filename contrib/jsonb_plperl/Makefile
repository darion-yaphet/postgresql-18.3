# contrib/jsonb_plperl/Makefile

MODULE_big = jsonb_plperl
OBJS = \
	$(WIN32RES) \
	jsonb_plperl.o
PGFILEDESC = "jsonb_plperl - jsonb transform for plperl"

PG_CPPFLAGS = -I$(top_srcdir)/src/pl/plperl

EXTENSION = jsonb_plperlu jsonb_plperl
DATA = jsonb_plperlu--1.0.sql jsonb_plperl--1.0.sql

REGRESS = jsonb_plperl jsonb_plperlu

SHLIB_LINK += $(filter -lm, $(LIBS))

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/jsonb_plperl
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# We must link libperl explicitly
#
# 我们必须显式链接 libperl
ifeq ($(PORTNAME), win32)
# these settings are the same as for plperl
#
# 这些设置与 plperl 的设置相同
override CPPFLAGS += -DPLPERL_HAVE_UID_GID -Wno-comment
# ... see silliness in plperl Makefile ...
#
# ...看看 plperl Makefile 中的愚蠢之处...
SHLIB_LINK_INTERNAL += $(sort $(wildcard ../../src/pl/plperl/libperl*.a))
else
rpathdir = $(perl_archlibexp)/CORE
SHLIB_LINK += $(perl_embed_ldflags)
endif

# As with plperl we need to include the perl_includespec directory last.
#
# 与 plperl 一样，我们需要最后包含 perl_includespec 目录。
override CPPFLAGS := $(CPPFLAGS) $(perl_embed_ccflags) $(perl_includespec)

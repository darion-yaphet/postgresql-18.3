# contrib/test_decoding/Makefile

MODULES = test_decoding
PGFILEDESC = "test_decoding - example of a logical decoding output plugin"

REGRESS = ddl xact rewrite toast permissions decoding_in_xact \
	decoding_into_rel binary prepared replorigin time messages \
	spill slot truncate stream stats twophase twophase_stream
ISOLATION = mxact delayed_startup ondisk_startup concurrent_ddl_dml \
	oldest_xmin snapshot_transfer subxact_without_top concurrent_stream \
	twophase_snapshot slot_creation_error catalog_change_snapshot \
	skip_snapshot_restore invalidation_distribution

REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/test_decoding/logical.conf
ISOLATION_OPTS = --temp-config $(top_srcdir)/contrib/test_decoding/logical.conf

# Disabled because these tests require "wal_level=logical", which
#
# 禁用，因为这些测试需要“wal_level=逻辑”，这
# typical installcheck users do not have (e.g. buildfarm clients).
#
# 典型的 installcheck 用户没有（例如 buildfarm 客户端）。
NO_INSTALLCHECK = 1

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/test_decoding
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# But it can nonetheless be very helpful to run tests on preexisting
#
# 但对预先存在的运行测试仍然非常有帮助
# installation, allow to do so, but only if requested explicitly.
#
# 安装，允许这样做，但前提是明确要求。
installcheck-force:
	$(pg_regress_installcheck) $(REGRESS)
	$(pg_isolation_regress_installcheck) $(ISOLATION)

# SPDX-License-Identifier: GPL-2.0

# "live" kunit tests
obj-$(CONFIG_DRM_XE_KUNIT_TEST) += xe_live_test.o
xe_live_test-y = xe_live_test_mod.o \
	xe_bo_test.o \
	xe_dma_buf_test.o \
	xe_migrate_test.o \
	xe_mocs_test.o

# Normal kunit tests
obj-$(CONFIG_DRM_XE_KUNIT_TEST) += xe_test.o
xe_test-y = xe_test_mod.o \
	xe_args_test.o \
	xe_pci_test.o \
	xe_rtp_test.o \
	xe_wa_test.o

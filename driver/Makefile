# SPDX-License-Identifier: GPL-2.0-only
#
# A kbuild fragment and nothing else. Out of tree it is copied into a build
# directory and used from there by tools/build_module.sh; in tree it would be
# drivers/accel/strela's Makefile. Neither case runs make from here, which is
# why there are no all:/clean: targets any more.

obj-$(CONFIG_DRM_ACCEL_STRELA) += strela2.o

strela2-y := \
	strela_core.o \
	strela_drv.o \
	strela_gem.o \
	strela_job.o

ccflags-y += -I$(src)/../include/uapi

ARCH          ?= arm
CROSS_COMPILE ?=

# NOTE: callers must pass CC for arm. This default expands to
# arm-linux-gnueabihf-gcc, which does not exist here — only the -13 suffixed
# one does. The top-level Makefile has ARM_CC for exactly this.
CC ?= $(CROSS_COMPILE)gcc

src  := $(CURDIR)/..

# KDIR is an O= build directory and KSRC is not: headers_install and
# gen_init_cpio are build products, while kselftest.h exists only in the source
# tree. Conflating the two is what broke this after the O= conversion.
KSRC ?= $(src)/3rdparty/linux-xlnx
KDIR ?= $(src)/qemu-kernel-$(ARCH)

# OUT, not O: O is kbuild's own output-directory variable, and a variable set on
# the command line propagates into every sub-make. Naming it O made the
# headers_install rule below inherit it and start laying a kernel build
# skeleton — arch/, include/, scripts/, usr/ — into our test output directory.
OUT ?= $(src)/qemu-run-$(ARCH)

MODULE ?=

CFLAGS      := -Wall -Wextra -O2
TEST_CFLAGS := $(CFLAGS) -Wno-unused-parameter \
	-I$(src)/include -I$(src)/include/uapi -I$(OUT)/uapi/include \
	-I$(KSRC)/tools/testing/selftests

LIBSTRELA ?= $(OUT)/lib/libstrela.a
LDLIBS    := -lstrela
LDFLAGS   := -static -L$(dir $(LIBSTRELA))

# gen_init_cpio ships with the kernel, so no cpio package is needed.
GEN_INIT_CPIO := $(KDIR)/usr/gen_init_cpio

.PHONY: all test_bin init initramfs clean

all: initramfs

test_bin:  $(OUT)/test_strela2
init:      $(OUT)/init
initramfs: $(OUT)/initramfs.cpio.gz

# Userspace needs the sanitized headers, not the raw ones in the kernel tree.
# Gated on one of its outputs so it runs once per output directory.
$(OUT)/uapi/include/drm/drm.h:
	@mkdir -p $(OUT)
	$(MAKE) -C $(KDIR) ARCH=$(ARCH) headers_install \
		INSTALL_HDR_PATH=$(OUT)/uapi > /dev/null

$(OUT)/test_strela2: test_strela2.c $(LIBSTRELA) $(OUT)/uapi/include/drm/drm.h
	@mkdir -p $(OUT)
	$(CC) $(TEST_CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/init: qemu_init.c
	@mkdir -p $(OUT)
	$(CC) -static $(CFLAGS) -o $@ $<

$(OUT)/initramfs.cpio.gz: $(OUT)/test_strela2 $(OUT)/init $(MODULE)
	@test -n "$(MODULE)" || \
		{ echo "initramfs needs MODULE=<path to strela2.ko>" >&2; exit 1; }
	@test -x "$(GEN_INIT_CPIO)" || \
		{ echo "no gen_init_cpio at $(GEN_INIT_CPIO); build that kernel first" >&2; exit 1; }
	printf '%s\n' \
		'dir /proc 755 0 0' \
		'dir /sys 755 0 0' \
		'dir /dev 755 0 0' \
		'nod /dev/console 600 0 0 c 5 1' \
		'file /init $(OUT)/init 755 0 0' \
		'file /test_strela2 $(OUT)/test_strela2 755 0 0' \
		'file /strela2.ko $(MODULE) 644 0 0' > $(OUT)/initramfs.list
	$(GEN_INIT_CPIO) $(OUT)/initramfs.list | gzip -9 > $@

clean:
	rm -f $(OUT)/test_strela2 $(OUT)/init
	rm -f $(OUT)/initramfs.list $(OUT)/initramfs.cpio.gz
	rm -rf $(OUT)/uapi

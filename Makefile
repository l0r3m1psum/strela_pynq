CROSS  ?= arm-linux-gnueabihf-
ARM_CC ?= $(CROSS)gcc-13

PYNQ         := $(CURDIR)/build-pynq
BOARD_KERNEL := $(PYNQ)/kernel/arch/arm/boot/zImage
BOARD_MODULE := $(PYNQ)/driver/strela2.ko
BOARD_TESTS  := $(PYNQ)/tests/test_strela2
BOARD_LIB    := $(PYNQ)/lib/libstrela.so
BOARD_LIB_A  := $(PYNQ)/lib/libstrela.a

ARM_TREE := $(CURDIR)/qemu-kernel-arm
X86_TREE := $(CURDIR)/qemu-kernel-x86_64
ARM_KERNEL   := $(ARM_TREE)/arch/arm/boot/zImage
X86_KERNEL   := $(X86_TREE)/arch/x86/boot/bzImage
ARM_DTB      := $(ARM_TREE)/arch/arm/boot/dts/xilinx/zynq-zc702.dtb

ARM_RUN := $(CURDIR)/qemu-run-arm
X86_RUN := $(CURDIR)/qemu-run-x86_64
ARM_MODULE := $(ARM_RUN)/build/strela2.ko
X86_MODULE := $(X86_RUN)/build/strela2.ko
ARM_LIB    := $(ARM_RUN)/lib/libstrela.a
X86_LIB    := $(X86_RUN)/lib/libstrela.a

DRIVER_SRC := $(wildcard driver/*.c driver/*.h)
LIB_SRC    := $(wildcard lib/*.c include/*.h)
TESTS      := $(MAKE) -C tests KSRC=$(CURDIR)/3rdparty/linux-xlnx
LIB        := $(MAKE) -C lib

.PHONY: all driver lib board-tests test test-arm test-x86_64 board-kernel test-kernels clean

all: driver lib board-tests

driver: $(BOARD_MODULE)

$(BOARD_MODULE): $(BOARD_KERNEL) $(DRIVER_SRC)
	tools/build_module.sh $(PYNQ)/kernel $(PYNQ)/driver arm $(CROSS) $(ARM_CC)

lib: $(BOARD_LIB) $(BOARD_LIB_A)

# Both flavours: the shared object for IREE, the archive for the tests.
$(BOARD_LIB) $(BOARD_LIB_A) &: $(LIB_SRC)
	$(LIB) CROSS_COMPILE=$(CROSS) CC=$(ARM_CC) OUT=$(PYNQ)/lib all

$(ARM_LIB): $(LIB_SRC)
	$(LIB) CROSS_COMPILE=$(CROSS) CC=$(ARM_CC) OUT=$(ARM_RUN)/lib static

$(X86_LIB): $(LIB_SRC)
	$(LIB) CC=gcc OUT=$(X86_RUN)/lib static

board-tests: $(BOARD_TESTS)

$(BOARD_TESTS): $(BOARD_KERNEL) $(BOARD_LIB_A) tests/test_strela2.c
	$(TESTS) ARCH=arm CROSS_COMPILE=$(CROSS) CC=$(ARM_CC) \
		KDIR=$(PYNQ)/kernel OUT=$(PYNQ)/tests LIBSTRELA=$(BOARD_LIB_A) test_bin

# TODO: test-board that download_device_tree_overlay.sh and run board tests

test: test-arm test-x86_64

test-arm: $(ARM_KERNEL) $(ARM_MODULE) $(ARM_LIB)
	$(TESTS) ARCH=arm CROSS_COMPILE=$(CROSS) CC=$(ARM_CC) \
		KDIR=$(ARM_TREE) OUT=$(ARM_RUN) MODULE=$(ARM_MODULE) \
		LIBSTRELA=$(ARM_LIB) initramfs
	tools/run_qemu.sh arm $(ARM_KERNEL) $(ARM_RUN)/initramfs.cpio.gz \
		$(ARM_RUN) $(ARM_DTB)

test-x86_64: $(X86_KERNEL) $(X86_MODULE) $(X86_LIB)
	$(TESTS) ARCH=x86_64 CC=gcc \
		KDIR=$(X86_TREE) OUT=$(X86_RUN) MODULE=$(X86_MODULE) \
		LIBSTRELA=$(X86_LIB) initramfs
	tools/run_qemu.sh x86_64 $(X86_KERNEL) $(X86_RUN)/initramfs.cpio.gz $(X86_RUN)

$(ARM_MODULE): $(ARM_KERNEL) $(DRIVER_SRC)
	tools/build_module.sh $(ARM_TREE) $(ARM_RUN)/build arm $(CROSS) $(ARM_CC)

$(X86_MODULE): $(X86_KERNEL) $(DRIVER_SRC)
	tools/build_module.sh $(X86_TREE) $(X86_RUN)/build x86_64 "" gcc

board-kernel: $(BOARD_KERNEL)
test-kernels: $(ARM_KERNEL) $(X86_KERNEL)

$(BOARD_KERNEL):
	tools/build_board_kernel.sh

$(ARM_KERNEL):
	tools/build_test_kernel.sh arm

$(X86_KERNEL):
	tools/build_test_kernel.sh x86_64

# The kernels are not touched: they take half an hour and rarely change.
clean:
	rm -rf $(PYNQ)/driver $(PYNQ)/lib $(PYNQ)/tests qemu-run-arm qemu-run-x86_64

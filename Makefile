CROSS  ?= arm-linux-gnueabihf-
ARM_CC ?= $(CROSS)gcc-13

PYNQ         := $(CURDIR)/build-pynq
BOARD_KERNEL := $(PYNQ)/kernel/arch/arm/boot/zImage
BOARD_MODULE := $(PYNQ)/driver/strela2.ko

ARM_KERNEL   := $(CURDIR)/qemu-kernel-arm/arch/arm/boot/zImage
X86_KERNEL   := $(CURDIR)/qemu-kernel-x86_64/arch/x86/boot/bzImage

.PHONY: all driver test test-arm test-x86_64 board-kernel test-kernels clean

all: driver

driver: $(BOARD_MODULE)

$(BOARD_MODULE): $(BOARD_KERNEL) $(wildcard driver/*.c driver/*.h)
	tools/build_module.sh $(PYNQ)/kernel $(PYNQ)/driver arm $(CROSS) $(ARM_CC)

test: test-arm test-x86_64

test-arm: $(ARM_KERNEL)
	ARCH=arm tools/run_qemu.sh

test-x86_64: $(X86_KERNEL)
	ARCH=x86_64 tools/run_qemu.sh

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
	rm -rf $(PYNQ)/driver qemu-run-arm qemu-run-x86_64

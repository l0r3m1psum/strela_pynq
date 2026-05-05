#!/bin/sh

set -e

make -C 3rdparty/linux-xlnx ARCH=arm xilinx_zynq_defconfig
grep -q "^CONFIG_STACKPROTECTOR_PER_TASK=y$" 3rdparty/linux-xlnx/.config || echo "CONFIG_STACKPROTECTOR_PER_TASK=y" >> 3rdparty/linux-xlnx/.config
make -C 3rdparty/linux-xlnx ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- CC=arm-linux-gnueabihf-gcc-13 modules_prepare
make -C 3rdparty/linux-xlnx ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- CC=arm-linux-gnueabihf-gcc-13 -j$(nproc) modules
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- CC=arm-linux-gnueabihf-gcc-13

# dtc -I fs -O dts /proc/devive-tree

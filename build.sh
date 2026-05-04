#!/bin/sh

set -e

make -C 3rdparty/linux-xlnx ARCH=arm xilinx_zynq_defconfig
grep -q "^CONFIG_STACKPROTECTOR_PER_TASK=y$" 3rdparty/linux-xlnx/.config || echo "CONFIG_STACKPROTECTOR_PER_TASK=y" >> 3rdparty/linux-xlnx/.config
make -C 3rdparty/linux-xlnx ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- CC=arm-linux-gnueabihf-gcc-13 modules_prepare
make -C 3rdparty/linux-xlnx ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- CC=arm-linux-gnueabihf-gcc-13 -j$(nproc) modules
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- CC=arm-linux-gnueabihf-gcc-13

# xsct generate_sdt.tcl design_1_wrapper.xsa sdt_out && printf "0a\n/dts-v1/;\n/plugin/;\n.\nw\n" | ed -s sdt_out/pl.dtsi && dtc -@ -I dts -O dtb -o pl.dtbo sdt_out/pl.dtsi
# unzip -o design_1_wrapper.xsa design_1_wrapper.bit && bootgen -image bitstream.bif -arch zynq -process_bitstream bin -w on

# dtc -I fs -O dts /proc/devive-tree

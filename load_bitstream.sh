#!/bin/sh

set -e

# unzip design_1_wrapper_lite.xsa -d design
# https://github.com/amaranth-lang/amaranth/issues/519
# write_cfgmem -force -format bin -interface smapx32 -disablebitswap -loadbit "up 0 {{name}}.bit" {{name}}.bin

# https://docs.amd.com/r/en-US/ug585-zynq-7000-SoC-TRM/AXI_HP-Interface-AFI-axi_hp
# 0xF8008000 base address
AXI_HP0=0xF8008000         # High-Performance AXI port 0 base register
AFI_RDCHAN_CTRL=0x00000000 # Read Channel Control Register
AFI_WRCHAN_CTRL=0x00000014 # Write Channel Control
# Configures both interfaces as 32bit instead of 64
# devmem2 $(printf 0x%x $(($AXI_HP0 + $AFI_RDCHAN_CTRL))) w 0x1
# devmem2 $(printf 0x%x $(($AXI_HP0 + $AFI_WRCHAN_CTRL))) w 0x1

full_bitstream=0
partial_bitstream=1

if [ -z "$1" ]
then
    echo "Usage: $0 <path_to_bitstream>"
    exit 1
fi

bitstream_path="$1"

if [ ! -f "$bitstream_path" ]
then
    echo "Error: File $bitstream_path not found."
    exit 1
fi

bitstream_name=$(basename "$bitstream_path")

echo $full_bitstream > /sys/class/fpga_manager/fpga0/flags
cp -f "$bitstream_path" /lib/firmware/
echo "$bitstream_name" > /sys/class/fpga_manager/fpga0/firmware

echo "Current FPGA State:"
cat /sys/class/fpga_manager/fpga0/state

# TODO: Device Tree Binary Overlay.
# mkdir -p /sys/kernel/config/device-tree/overlays/my_overlay
# cat my_overlay.dtbo > /sys/kernel/config/device-tree/overlays/my_overlay/dtbo

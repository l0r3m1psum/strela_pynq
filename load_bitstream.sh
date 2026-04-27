#!/bin/sh

set -e

# unzip design_1_wrapper_lite.xsa -d design
# https://github.com/amaranth-lang/amaranth/issues/519
# write_cfgmem -force -format bin -interface smapx32 -disablebitswap -loadbit "up 0 {{name}}.bit" {{name}}.bin


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

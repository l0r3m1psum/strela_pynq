#!/bin/sh

set -e

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
cat /sys/class/fpga_manager/fpga0/state

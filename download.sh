#!/bin/sh

set -e

name=$1
addr=10.100.4.202

# https://docs.amd.com/r/en-US/ug585-zynq-7000-SoC-TRM/AXI_HP-Interface-AFI-axi_hp
AXI_HP0=0xF8008000         # High-Performance AXI port 0 base register
AFI_RDCHAN_CTRL=0x00000000 # Read Channel Control Register
AFI_WRCHAN_CTRL=0x00000014 # Write Channel Control
FIELD_32BitEn=0x1          # Enables the 32-bit interface (as opposed to 64-bit)
FIELD_WrDataThreshold=0xF00 # It was originally there...

scp "${name}.bit.bin" "${name}.dtbo" root@${addr}:/lib/firmware
scp src/strela.ko root@${addr}:/root
scp src/xilinx-afi.ko root@${addr}:/root
scp src/test_bypass root@${addr}:/root

# Assuming configfs is mounted
# mount -t configfs none /sys/kernel/config
ssh root@${addr} << EOF
	set -x

	rmmod strela
	rmmod xilinx-afi

	# This should be handled by the "FPGA Region" but it is not...
	devmem2 $(printf 0x%x $(($AXI_HP0 + $AFI_RDCHAN_CTRL))) w $FIELD_32BitEn
	devmem2 $(printf 0x%x $(($AXI_HP0 + $AFI_WRCHAN_CTRL))) w $(printf 0x%x $(($FIELD_WrDataThreshold | $FIELD_32BitEn)))

	if [ -d /sys/kernel/config/device-tree/overlays/strela ]
	then
		rmdir /sys/kernel/config/device-tree/overlays/strela
	fi
	mkdir -p /sys/kernel/config/device-tree/overlays/strela
	echo "${name}.dtbo" >/sys/kernel/config/device-tree/overlays/strela/path
	cat /sys/kernel/config/device-tree/overlays/strela/status

	# This module doesn't do what it should i.e configure AXI FIFO Interface
	# with information from the device tree...
	insmod xilinx-afi.ko
	insmod strela.ko

	ls /sys/class/strela
	grep strela /proc/devices
	grep axi_lite /proc/iomem
	ls /dev/strela*

	dmesg | tail -n 20
	./test_bypass
EOF

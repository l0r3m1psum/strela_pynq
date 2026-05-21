#!/bin/sh

set -e

# TODO: make distinction between path and name.
name=$1
addr=10.100.4.202

# https://docs.amd.com/r/en-US/ug585-zynq-7000-SoC-TRM/AXI_HP-Interface-AFI-axi_hp
AXI_HP0=0xF8008000         # High-Performance AXI port 0 base register
AFI_RDCHAN_CTRL=0x00000000 # Read Channel Control Register
AFI_WRCHAN_CTRL=0x00000014 # Write Channel Control
FIELD_32BitEn=0x1          # Enables the 32-bit interface (as opposed to 64-bit)
FIELD_WrDataThreshold=0xF00 # It was originally there...

scp "${name}.bit.bin" "${name}.dtbo" root@${addr}:/lib/firmware
scp driver/strela.ko root@${addr}:/root
scp lib/libstrela.so root@${addr}:/usr/local/lib
scp tools/test_strela root@${addr}:/root
scp tools/test_bypass root@${addr}:/root

# Assuming configfs is mounted
# mount -t configfs none /sys/kernel/config
ssh root@${addr} << EOF
	set -ex

	rmmod strela || true
	ldconfig

	# devmem2 $(printf 0x%x $(($AXI_HP0 + $AFI_RDCHAN_CTRL))) w $FIELD_32BitEn
	# devmem2 $(printf 0x%x $(($AXI_HP0 + $AFI_WRCHAN_CTRL))) w $(printf 0x%x $(($FIELD_WrDataThreshold | $FIELD_32BitEn)))

	if [ -d /sys/kernel/config/device-tree/overlays/strela ]
	then
		rmdir /sys/kernel/config/device-tree/overlays/strela
	fi
	mkdir -p /sys/kernel/config/device-tree/overlays/strela
	echo "${name}.dtbo" >/sys/kernel/config/device-tree/overlays/strela/path
	cat /sys/kernel/config/device-tree/overlays/strela/status

	insmod strela.ko

	ls /sys/class/strela
	grep strela /proc/devices
	grep axi_lite /proc/iomem
	ls /dev/strela*

	dmesg | tail -n 20
	./test_bypass
	./test_strela
	dmesg | tail -n 20
EOF

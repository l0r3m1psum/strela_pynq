#!/bin/sh

set -e

name=$1
addr=10.100.4.202

scp "${name}.bit.bin" "${name}.dtbo" root@${addr}:/lib/firmware
scp src/strela.ko root@${addr}:/root
scp src/xilinx-afi.ko root@${addr}:/root
scp src/test_bypass root@${addr}:/root

# Assuming configfs is mounted
# mount -t configfs none /sys/kernel/config
ssh root@${addr} << EOF
	rmmod strela
	rmmod xilinx-afi

	if [ -d /sys/kernel/config/device-tree/overlays/strela ]
	then
		rmdir /sys/kernel/config/device-tree/overlays/strela
	fi
	mkdir -p /sys/kernel/config/device-tree/overlays/strela
	echo "${name}.dtbo"
	echo "${name}.dtbo" >/sys/kernel/config/device-tree/overlays/strela/path
	printf "status: " && cat /sys/kernel/config/device-tree/overlays/strela/status

	insmod xilinx-afi.ko
	insmod strela.ko

	ls /sys/class/strela
	grep strela /proc/devices
	grep axi_lite /proc/iomem
	ls /dev/strela*

	echo dmesg
	dmesg | tail -n 20
	./test_bypass
EOF

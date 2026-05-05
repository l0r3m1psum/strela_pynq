#!/bin/sh

set -e

name=$1
addr=10.100.4.202

scp "${name}.bit.bin" "${name}.dtbo" root@${addr}:/lib/firmware
scp src/strela.ko root@${addr}:/root

# Assuming configfs is mounted
# mount -t configfs none /sys/kernel/config
ssh root@${addr} << EOF
	rmmod strela
	insmod strela.ko

	if [ -d /sys/kernel/config/device-tree/overlays/strela ]
	then
		rmdir /sys/kernel/config/device-tree/overlays/strela
	fi
	mkdir -p /sys/kernel/config/device-tree/overlays/strela
	echo "${name}.dtbo"
	echo "${name}.dtbo" >/sys/kernel/config/device-tree/overlays/strela/path
	printf "status: " && cat /sys/kernel/config/device-tree/overlays/strela/status
	echo dmesg
	dmesg | tail
EOF

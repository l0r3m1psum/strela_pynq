#!/bin/sh

set -e

name="${1:-design_1_wrapper_2_cgras_int_fix}"

if [ -z "${BOARD_IP_ADDR}" ]
then
	echo "The environment variable BOARD_IP_ADDR is not set." >&2
	exit 1
fi

scp "${name}.bit.bin" "${name}.dtbo" root@${BOARD_IP_ADDR}:/lib/firmware
scp build-pynq/driver/strela2.ko root@${BOARD_IP_ADDR}:/root

ssh root@${BOARD_IP_ADDR} << EOF
	set -ex

	rmmod strela2 || true
	# ldconfig

	if [ -d /sys/kernel/config/device-tree/overlays/strela ]
	then
		rmdir /sys/kernel/config/device-tree/overlays/strela
	fi
	mkdir -p /sys/kernel/config/device-tree/overlays/strela
	echo "${name}.dtbo" >/sys/kernel/config/device-tree/overlays/strela/path
	cat /sys/kernel/config/device-tree/overlays/strela/status

	insmod strela2.ko

	ls /sys/class/accel
	grep accel /proc/devices
	grep axi_lite /proc/iomem
	ls /dev/accel
EOF

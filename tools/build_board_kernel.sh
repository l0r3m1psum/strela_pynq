#!/bin/sh

set -e

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
ktree=$src/3rdparty/linux-xlnx
out=${OUT:-$src/build-pynq/kernel}
cross=${CROSS_COMPILE:-arm-linux-gnueabihf-}
cc=${CC:-${cross}gcc-13}
jobs=${JOBS:-$(nproc)}

"$here/kconfig.sh" "$ktree" "$out" arm "$cross" "$cc" xilinx_zynq_defconfig \
	"$here/config/module.config" \
	"$here/config/arm.config"

# LOADADDR has to be the same as "Load Address" and "Entry Point" for the real
# board mkimage -l /boot/uImage
make -C "$ktree" O="$out" ARCH=arm CROSS_COMPILE="$cross" CC="$cc" \
	LOADADDR=0x8000 -j"$jobs" zImage dtbs modules uImage
echo "kernel: $out/arch/arm/boot/zImage"

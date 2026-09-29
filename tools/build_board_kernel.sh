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

# modules as well as the image: an out-of-tree module build needs this build's
# Module.symvers to link against.
make -C "$ktree" O="$out" ARCH=arm CROSS_COMPILE="$cross" CC="$cc" \
	-j"$jobs" zImage dtbs modules
echo "kernel: $out/arch/arm/boot/zImage"

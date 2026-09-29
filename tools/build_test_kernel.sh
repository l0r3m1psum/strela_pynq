#!/bin/sh

set -e

arch=${1:-}
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
ktree=$src/3rdparty/linux-xlnx
out=${OUT:-$src/qemu-kernel-$arch}
jobs=${JOBS:-$(nproc)}

case $arch in
arm)
	defconfig=xilinx_zynq_defconfig
	cross=${CROSS_COMPILE:-arm-linux-gnueabihf-}
	cc=${CC:-${cross}gcc-13}
	targets="zImage dtbs modules"
	imagepath=arch/arm/boot/zImage
	;;
x86_64)
	defconfig=x86_64_defconfig
	cross=${CROSS_COMPILE:-}
	cc=${CC:-gcc}
	targets="bzImage modules"
	imagepath=arch/x86/boot/bzImage
	;;
*)
	echo "usage: $0 <arm|x86_64>" >&2
	exit 2
	;;
esac

"$here/kconfig.sh" "$ktree" "$out" "$arch" "$cross" "$cc" "$defconfig" \
	"$here/config/module.config" \
	"$here/config/test.config" \
	"$here/config/$arch.config"

# shellcheck disable=SC2086  # $targets is a deliberate word list
make -C "$ktree" O="$out" ARCH="$arch" CROSS_COMPILE="$cross" CC="$cc" \
	-j"$jobs" $targets
echo "kernel: $out/$imagepath"

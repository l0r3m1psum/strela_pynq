#!/bin/sh
# Build strela2.ko out of tree, against an already-configured kernel build.
#
#   tools/build_module.sh <kernel-build-dir> <output-dir> [arch] [cross] [cc]
#
# The sources are copied into <output-dir> and built there. 6.12's kbuild has
# no MO= to send external-module output elsewhere, so copying is the only way
# to keep driver/ free of .o files and every build's output under one directory
# that can be deleted whole. tools/run_qemu.sh needs exactly the same thing for
# each test target, so it calls this too.

set -e

kbuild=${1:?usage: $0 <kernel-build-dir> <output-dir> [arch] [cross] [cc]}
out=${2:?usage: $0 <kernel-build-dir> <output-dir> [arch] [cross] [cc]}
arch=${3:-arm}
cross=${4-arm-linux-gnueabihf-}
cc=${5:-${cross}gcc-13}
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)

mkdir -p "$out"
cp "$src/driver/Makefile" "$src"/driver/strela_*.c "$src"/driver/strela_*.h \
	"$src/include/uapi/strela_drm.h" "$out/"
# The old char-device driver is not part of this module.
rm -f "$out/strela_driver.c"

make -C "$kbuild" M="$out" ARCH="$arch" CROSS_COMPILE="$cross" CC="$cc" \
	CONFIG_DRM_ACCEL_STRELA=m modules
echo "module: $out/strela2.ko"

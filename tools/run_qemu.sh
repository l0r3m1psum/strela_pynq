#!/bin/sh

set -e

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
# $kdir below is an O= build directory, so anything that lives in the sources
# rather than the build — kselftest.h, for one — has to come from here.
ktree=$src/3rdparty/linux-xlnx
arch=${ARCH:-arm}
out=${OUT:-$src/qemu-run-$arch}

case $arch in
arm)
	kdir=${KDIR:-$src/qemu-kernel-arm}
	cross=arm-linux-gnueabihf-
	cc=${cross}gcc-13
	qemu=qemu-system-arm
	kernel=$kdir/arch/arm/boot/zImage
	kbuild_arch=arm
	;;
x86_64)
	kdir=${KDIR:-$src/qemu-kernel-x86_64}
	cross=
	cc=gcc
	qemu=qemu-system-x86_64
	kernel=$kdir/arch/x86/boot/bzImage
	kbuild_arch=x86_64
	;;
*)
	echo "unknown ARCH=$arch (use arm or x86_64)" >&2
	exit 1
	;;
esac

case "$arch:$(uname -m)" in
x86_64:x86_64 | arm:armv7* | arm:armv8l) kvm_possible=yes ;;
*)                                       kvm_possible=no ;;
esac
if [ "$kvm_possible" = yes ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
	accel=kvm
else
	accel=tcg
fi

rm -rf "$out"
mkdir -p "$out/build"

# The module, built out of tree against that kernel. Same script the board's
# module uses, so the two cannot drift.
"$here/build_module.sh" "$kdir" "$out/build" "$kbuild_arch" "$cross" "$cc"

# Userspace needs the sanitized headers, not the raw ones in the kernel tree.
make -C "$kdir" ARCH=$kbuild_arch headers_install INSTALL_HDR_PATH="$out/uapi" > /dev/null

$cc -static -Wall -Wextra -Wno-unused-parameter -O2 \
	-I"$src/include/uapi" -I"$out/uapi/include" \
	-I"$ktree/tools/testing/selftests" \
	-o "$out/test_strela2" "$src/tools/test_strela2.c"
$cc -static -Wall -Wextra -O2 -o "$out/init" "$src/tools/qemu_init.c"

# Initramfs. gen_init_cpio ships with the kernel, so no cpio package is needed.
gen_init_cpio=$kdir/usr/gen_init_cpio
[ -x "$gen_init_cpio" ] || gen_init_cpio=$src/build-pynq/kernel/usr/gen_init_cpio
cat > "$out/initramfs.list" <<EOF
dir /proc 755 0 0
dir /sys 755 0 0
dir /dev 755 0 0
nod /dev/console 600 0 0 c 5 1
file /init $out/init 755 0 0
file /test_strela2 $out/test_strela2 755 0 0
file /strela2.ko $out/build/strela2.ko 644 0 0
EOF
"$gen_init_cpio" "$out/initramfs.list" | gzip -9 > "$out/initramfs.cpio.gz"

echo "booting ($arch, $accel)..."
if [ "$arch" = arm ]; then
	# Zynq's console is UART1, which is QEMU's *second* serial port, so the
	# first one goes to null and the second to stdio.
	timeout 180 $qemu \
		-M xilinx-zynq-a9 \
		-accel $accel \
		-m 512 \
		-display none \
		-serial null \
		-serial mon:stdio \
		-no-reboot \
		-kernel "$kernel" \
		-dtb "$kdir/arch/arm/boot/dts/xilinx/zynq-zc702.dtb" \
		-initrd "$out/initramfs.cpio.gz" \
		-append "console=ttyPS0,115200 earlyprintk rdinit=/init panic=1" \
		< /dev/null | tee "$out/console.log"
else
	[ "$accel" = kvm ] && cpu=host || cpu=max
	timeout 180 $qemu \
		-M q35 \
		-accel $accel \
		-cpu $cpu \
		-m 512 \
		-display none \
		-serial mon:stdio \
		-no-reboot \
		-kernel "$kernel" \
		-initrd "$out/initramfs.cpio.gz" \
		-append "console=ttyS0 rdinit=/init panic=1" \
		< /dev/null | tee "$out/console.log"
fi

echo

# Strip kernel log
sed -E 's/^\[[ ]*[0-9]+\.[0-9]+\] //' "$out/console.log" > "$out/tap.log"

# A kernel splat fails the run even when every test says ok. The driver's
# calling contracts are WARN_ON and lockdep assertions, and a WARN prints
# without failing anything by itself — so without this they catch nothing.
#
# One exception, an upstream false positive: dma_alloc_pages() users never call
# dma_mapping_error(), so the debug entry stays MAP_ERR_NOT_CHECKED and
# check_unmap() complains on every free.
splat=$(grep -E '^(WARNING:|BUG:|Kernel panic)' "$out/tap.log" |
	grep -v 'check_unmap' || true)
if [ -n "$splat" ]; then
	echo "$splat" >&2
	echo "FAIL: kernel splat (full log: $out/console.log)" >&2
	exit 1
fi

# Anything not ok, or tests that did not run to completion, is a failure.
if grep -q '^not ok' "$out/tap.log"; then
	grep '^not ok' "$out/tap.log" >&2
	echo "FAIL (full log: $out/console.log)" >&2
	exit 1
fi
if ! grep -q "qemu_init: tests exited 0" "$out/console.log"; then
	echo "FAIL: tests did not complete (full log: $out/console.log)" >&2
	exit 1
fi
grep -c '^ok' "$out/tap.log" | sed 's/^/passed: /'
echo "PASS (full log: $out/console.log)"

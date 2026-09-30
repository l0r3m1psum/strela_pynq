#!/bin/sh
# Boot a kernel under QEMU with an initramfs, and decide whether the run passed.
#
#   tools/run_qemu.sh <arch> <kernel> <initramfs> <outdir> [dtb]

set -e

arch=${1:?usage: $0 <arch> <kernel> <initramfs> <outdir> [dtb]}
kernel=${2:?usage: $0 <arch> <kernel> <initramfs> <outdir> [dtb]}
initramfs=${3:?usage: $0 <arch> <kernel> <initramfs> <outdir> [dtb]}
out=${4:?usage: $0 <arch> <kernel> <initramfs> <outdir> [dtb]}
dtb=$5

case $arch in
arm)	qemu=qemu-system-arm ;;
x86_64)	qemu=qemu-system-x86_64 ;;
*)	echo "unknown arch '$arch' (use arm or x86_64)" >&2; exit 1 ;;
esac

case "$arch:$(uname -m)" in
x86_64:x86_64 | arm:armv7* | arm:armv8l) kvm_possible=yes ;;
*)                                       kvm_possible=no ;;
esac
if [ "$kvm_possible" = yes ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
	accel=kvm
else
	accel=tcg
	[ "$kvm_possible" = no ] ||
		echo "note: /dev/kvm not usable, falling back to emulation" >&2
fi

mkdir -p "$out"
echo "booting ($arch, $accel)..."
if [ "$arch" = arm ]; then
	[ -f "$dtb" ] || { echo "arm needs a dtb; got '$dtb'" >&2; exit 1; }
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
		-dtb "$dtb" \
		-initrd "$initramfs" \
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
		-initrd "$initramfs" \
		-append "console=ttyS0 rdinit=/init panic=1" \
		< /dev/null | tee "$out/console.log"
fi

echo

# Both the userspace tests and KUnit emit TAP, but KUnit's goes through the
# kernel log, so it carries a "[    2.345678] " prefix wherever printk
# timestamps are on and none where they are not. Judging the raw log therefore
# gave the two targets different totals for the same tests — and, worse, let a
# KUnit failure pass unnoticed on whichever target had the prefix. Strip it and
# treat both the same.
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

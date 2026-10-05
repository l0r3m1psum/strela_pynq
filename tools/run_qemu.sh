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
		-append "console=ttyPS0,115200 earlyprintk rdinit=/init panic=1 panic_on_warn=1 oops=panic quiet" \
		< /dev/null | tee "$out/console.log"
else
	[ "$accel" = kvm ] && cpu=host || cpu=max
	timeout 180 $qemu \
		-M q35 \
		-nodefaults \
		-accel $accel \
		-cpu $cpu \
		-m 512 \
		-display none \
		-serial mon:stdio \
		-no-reboot \
		-kernel "$kernel" \
		-initrd "$initramfs" \
		-append "console=ttyS0 rdinit=/init panic=1 panic_on_warn=1 oops=panic quiet" \
		< /dev/null | tee "$out/console.log"
fi

echo

# panic_on_warn stops the VM at the first kernel warning, so a splat is the last
# thing on the console and qemu_init never reports. KUnit's results only reach
# the kernel log, with a timestamp in front wherever printk has them on.
ts='^(\[[ 0-9.]*\] )?'
if grep -q "qemu_init: tests exited 0" "$out/console.log" &&
   ! grep -Eq "$ts *not ok " "$out/console.log"; then
	grep -Ec "${ts}ok " "$out/console.log" | sed 's/^/passed: /'
	echo "PASS (full log: $out/console.log)"
	exit 0
fi

{
	echo "==================== test output ===================="
	grep -E "$ts *(#|not ok) " "$out/console.log" || true
	echo
	echo "==================== end of the console ===================="
	tail -n 80 "$out/console.log"
	echo
	echo "FAIL (full log: $out/console.log)"
} >&2
exit 1

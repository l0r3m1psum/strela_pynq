#!/bin/sh
# Legacy entry point, for the pre-accel char-device driver and the userspace
# that links against it: lib/libstrela.so, tools/test_bypass, tools/test_strela.
# Nothing else builds these.
#
# The accel driver and its tests are the top-level Makefile's job now; see
# `make help`. This script still builds the kernel and the accel module too, so
# that it remains a single command for a board from scratch.

set -e

cc=arm-linux-gnueabihf-gcc-13
cflags="-Wall -Wextra -g -I include"

tools/build_board_kernel.sh
tools/build_module.sh build-pynq/kernel build-pynq/driver arm arm-linux-gnueabihf- "$cc"

$cc $cflags -I include/uapi -shared lib/strela.c -o lib/libstrela.so
$cc $cflags -I include/uapi tools/test_bypass.c -o tools/test_bypass
$cc $cflags tools/test_strela.c -o tools/test_strela -L lib -l strela

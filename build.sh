#!/bin/sh

set -e

cc=arm-linux-gnueabihf-gcc-13
cflags="-Wall -Wextra -g -I include"

(
	cd driver
	./build.sh
)
$cc $cflags tools/test_strela.c -o tools/test_strela
$cc $cflags -I include/uapi -shared lib/strela.c -o lib/libstrela.so

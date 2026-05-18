#!/bin/sh

set -e

cc=arm-linux-gnueabihf-gcc-13

(
	cd driver
	./build.sh
)
$cc -g -I include/uapi tools/test_bypass.c -o tools/test_bypass
$cc -g -I include/uapi -I include -shared lib/strela.c -o lib/strela.so

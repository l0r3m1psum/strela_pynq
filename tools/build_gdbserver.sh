#!/bin/sh

set -e

# TODO(Diego): parametrize for ARM build

wget https://ftp.gnu.org/gnu/gdb/gdb-15.1.tar.xz
tar xf gdb-15.1.tar.xz
mkdir build-gdbserver
cd build-gdbserver
../gdb-15.1/configure \
	--disable-gdb \
	--disable-sim \
	--disable-ld \
	--disable-gas \
	--disable-binutils \
	--disable-gprof \
	--disable-inprocess-agent \
	--enable-gdbserver \
	LDFLAGS=-static
make -j8 all-gdbserver
file gdbserver/gdbserver

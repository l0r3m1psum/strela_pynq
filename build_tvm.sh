#!/bin/sh

set -e

addr="10.100.4.202"
strela_include="$PWD/include"
strela_lib="$PWD/lib"

[ -d venv ] || python3 -m venv venv
. venv/bin/activate
(
	mkdir -p 3rdparty/tvm/build
	cd 3rdparty/tvm/build
	cp ../cmake/config.cmake .
	echo "set(TVM_FFI_BUILD_PYTHON_MODULE ON)" >> config.cmake
	echo "set(USE_STRELA_CODEGEN ON)" >> config.cmake
	echo "set(USE_STRELA_RUNTIME ON)" >> config.cmake
	echo "add_compile_options(-Wno-psabi)" >> config.cmake
	echo "set(STRELA_INCLUDE_DIR $strela_include)" >> config.cmake
	echo "set(STRELA_LIB_DIR $strela_lib)" >> config.cmake
	cmake .. \
		-DCMAKE_SYSTEM_NAME=Linux \
		-DCMAKE_SYSTEM_PROCESSOR=arm \
		-DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc-13 \
		-DCMAKE_CXX_COMPILER=arm-linux-gnueabihf-g++-13
	cmake --build . --parallel $(nproc)
	cd ../3rdparty/tvm-ffi
	CC=arm-linux-gnueabihf-gcc-13 \
	CXX=arm-linux-gnueabihf-g++-13 \
	_PYTHON_HOST_PLATFORM=linux-armv7l \
	CFLAGS="-I $HOME/arm-sysroot/usr/include $CFLAGS" \
	CXXFLAGS="-I $HOME/arm-sysroot/usr/include $CXXFLAGS" \
		pip wheel .
)
tar -czf - 3rdparty/tvm | ssh root@$addr "cat > tvm.tar.gz"

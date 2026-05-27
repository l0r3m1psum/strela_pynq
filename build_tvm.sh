#!/bin/sh

set -e

addr="10.100.4.202"

[ -d venv ] || python3 -m venv venv
. venv/bin/activate
(
	mkdir -p 3rdparty/tvm/build
	cd 3rdparty/tvm/build
	cp ../cmake/config.cmake .
	echo "set(TVM_FFI_BUILD_PYTHON_MODULE ON)" >> config.cmake
	cmake .. \
		-DCMAKE_SYSTEM_NAME=Linux \
		-DCMAKE_SYSTEM_PROCESSOR=arm \
		-DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc-13 \
		-DCMAKE_CXX_COMPILER=arm-linux-gnueabihf-g++-13
	cmake --build . --parallel $(nproc)
	cd ../3rdparty/tvm-ffi
	CFLAGS="-I $HOME/arm-sysroot/usr/include $CFLAGS" \
		CXXFLAGS="-I $HOME/arm-sysroot/usr/include $CXXFLAGS" \
		pip wheel .
	mv -f apache_tvm_ffi-0.1.11-cp312-abi3-linux_x86_64.whl \
		apache_tvm_ffi-0.1.11-cp312-abi3-linux_armv7l.whl
)
tar -czf - 3rdparty/tvm | ssh root@$addr "cat > tvm.tar.gz"

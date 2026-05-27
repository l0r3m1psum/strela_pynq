#!/bin/sh

addr=10.100.4.202

ssh root@${addr} << EOF
	set -e

	if [ ! -d 3rdparty ]
	then
		tar -xzf tvm.tar.gz
	fi

	if [ ! -d venv ]
	then
		python3 -m venv venv
		. venv/bin/activate
		pip install numpy psutil pytest cloudpickle
		pip install 3rdparty/tvm/3rdpary/tvm-ffi/apache_tvm_ffi-0.1.11-cp312-abi3-linux_armv7l.whl
	else
		. venv/bin/activate
	fi

	cd 3rdparty/tvm
	# Here $PWD is escaped to make it expand on the remote system
	PYTHONPATH=\$PWD/python python3 -m tvm.exec.rpc_server --host 0.0.0.0 --port=9090
EOF

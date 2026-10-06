#!/bin/sh

if [ -z "${BOARD_IP_ADDR}" ]
then
	echo "The environment variable BOARD_IP_ADDR is not set." >&2
	exit 1
fi

scp build-pynq/tests/test_strela2 \
	build-pynq/lib/libstrela.so \
	root@${BOARD_IP_ADDR}:/root

ssh root@${BOARD_IP_ADDR} << EOF
	set -ex

	./test_strela2
	./test_strela2 library
EOF

#!/bin/sh

set -e

if [ -z "${BOARD_IP_ADDR}" ]
then
	echo "The environment variable BOARD_IP_ADDR is not set." >&2
	exit 1
fi

scp -r board/etc root@${BOARD_IP_ADDR}:/

ssh root@${BOARD_IP_ADDR} << EOF
	set -ex

	sysctl --system >/dev/null
	systemctl daemon-reexec

	sysctl kernel.panic_on_oops kernel.panic
	systemctl show -p RuntimeWatchdogUSec -p RebootWatchdogUSec
EOF

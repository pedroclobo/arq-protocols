#!/bin/bash

set -euo pipefail

rm -f send.dat receive.dat sender-packets.log receiver-packets.log
dd if=/dev/urandom of=send.dat bs=1000 count=5 >/dev/null 2>&1

LOG_PACKETS=$(realpath ./bin/log-packets.so)
FILE_SENDER=$(realpath ./bin/file-sender)
FILE_RECEIVER=$(realpath ./bin/file-receiver)

LD_PRELOAD="$LOG_PACKETS" \
PACKET_LOG="sender-packets.log" \
DROP_PATTERN="" \
$FILE_SENDER 1234 3 > /dev/null 2>&1 &

SENDER_PID=$!
sleep .1

pushd /tmp > /dev/null
	LD_PRELOAD="$LOG_PACKETS" \
	PACKET_LOG="receiver-packets.log" \
	DROP_PATTERN="" \
	$FILE_RECEIVER send.dat localhost 1234 1 > /dev/null 2>&1 || true
popd > /dev/null

wait $SENDER_PID || true

mv /tmp/send.dat receive.dat

SCRIPT_NAME=$(basename "$0")
if ! diff send.dat receive.dat > /dev/null 2>&1; then
	echo -e "\e[31m$SCRIPT_NAME: FAILED\e[0m"
else
	echo -e "\e[32m$SCRIPT_NAME: PASSED\e[0m"
fi

rm send.dat receive.dat

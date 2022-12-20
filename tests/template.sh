#!/bin/bash

# $1 = test name
# $2 = bs
# $3 = count
# $4 = sender drop pattern
# $5 = sender window size
# $6 = receiver drop pattern
# $7 = receiver window size

rm -f send.dat receive.dat sender-packets.log receiver-packets.log
dd if=/dev/urandom of=send.dat bs="$2" count="$3" >/dev/null 2>&1

LOG_PACKETS=$(realpath ./bin/log-packets.so)
FILE_SENDER=$(realpath ./bin/file-sender)
FILE_RECEIVER=$(realpath ./bin/file-receiver)

LD_PRELOAD="$LOG_PACKETS" \
PACKET_LOG="sender-packets.log" \
DROP_PATTERN="$4" \
$FILE_SENDER 1234 "$5" > /dev/null 2>&1 &

SENDER_PID=$!
sleep .1

pushd /tmp > /dev/null || exit
	LD_PRELOAD="$LOG_PACKETS" \
	PACKET_LOG="receiver-packets.log" \
	DROP_PATTERN="$6" \
	$FILE_RECEIVER send.dat localhost 1234 "$7" > /dev/null 2>&1

	RECEIVER_EXIT_CODE=$?
popd > /dev/null || exit

wait $SENDER_PID
SENDER_EXIT_CODE=$?

mv /tmp/send.dat receive.dat

if ! diff send.dat receive.dat > /dev/null 2>&1; then
	echo -e "\e[31m$1: FAILED\e[0m: Files differ"
elif [ $RECEIVER_EXIT_CODE -ne 0 ] && [ $SENDER_EXIT_CODE -ne 0 ]; then
	echo -e "\e[31m$1: FAILED\e[0m: Reciever and Sender exited with EXIT_FAILURE"
elif [ $RECEIVER_EXIT_CODE -ne 0 ]; then
	echo -e "\e[31m$1: FAILED\e[0m: Reciever exited with EXIT_FAILURE"
elif [ $SENDER_EXIT_CODE -ne 0 ]; then
	echo -e "\e[31m$1: FAILED\e[0m: Sender exited with EXIT_FAILURE"
else
	echo -e "\e[32m$1: PASSED\e[0m"
fi

rm send.dat receive.dat

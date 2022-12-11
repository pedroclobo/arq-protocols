#!/bin/bash

for file in ./tests/*.sh; do
	if [ "$file" != "./tests/run.sh" ]; then
		$file
		TEST_PID=$!
		wait $TEST_PID
	fi
done

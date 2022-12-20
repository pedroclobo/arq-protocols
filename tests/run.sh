#!/bin/bash

while read -r line; do
	IFS=':' read -r -a array <<< "$line"
	./tests/template.sh "${array[0]}" "${array[1]}" "${array[2]}" "${array[3]}" "${array[4]}" "${array[5]}" "${array[6]}"
	wait $!
done < tests/tests

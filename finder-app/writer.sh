#!/bin/bash

writefile=${1}
writestr=${2}

test $# -lt 2 && exit 1

mkdir -p $(dirname ${writefile})
echo "${writestr[@]}" > ${writefile}

exit 0

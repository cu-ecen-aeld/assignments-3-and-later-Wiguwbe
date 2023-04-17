#!/bin/sh

set -e

filesdir=${1}
searchstr=${2}

test $# -lt 2 && exit 1

test -d $filesdir || exit 1

m_lines=$(grep -r ${searchstr} ${filesdir} | wc -l)
n_files=$(find ${filesdir} -type f | wc -l)

echo "The number of files are ${n_files} and the number of matching lines are ${m_lines}"

exit 0

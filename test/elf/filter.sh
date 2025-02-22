#!/bin/bash
export LANG=
set -e
CC="${CC:-cc}"
CXX="${CXX:-c++}"
testname=$(basename -s .sh "$0")
echo -n "Testing $testname ... "
cd "$(dirname "$0")"/../..
mold="$(pwd)/mold"
t=out/test/elf/$testname
mkdir -p $t

cat <<EOF | $CC -o $t/a.o -c -x assembler -
  .text
  .globl _start
_start:
  nop
EOF

"$mold" -o $t/b.so $t/a.o --filter foo -F bar -shared

readelf --dynamic $t/b.so > $t/log
fgrep -q 'Filter library: [foo]' $t/log
fgrep -q 'Filter library: [bar]' $t/log

echo OK

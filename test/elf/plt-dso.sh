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

cat <<EOF | $CC -fPIC -c -o $t/a.o -xc -
#include <stdio.h>

void world() {
  printf("world\n");
}

void real_hello() {
  printf("Hello ");
  world();
}

void hello() {
  real_hello();
}
EOF

$CC -B. -shared -o $t/b.so $t/a.o

cat <<EOF | $CC -c -o $t/c.o -xc -
#include <stdio.h>

void world() {
  printf("WORLD\n");
}

void hello();

int main() {
  hello();
}
EOF

$CC -B. -o $t/exe -Wl,-rpath=$t $t/c.o $t/b.so
$t/exe | grep -q 'Hello WORLD'

echo OK

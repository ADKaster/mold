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

cat <<'EOF' | $CC -c -o $t/a.o -xc -
__attribute__((section("foo")))
char data[] = "section foo";
EOF

ar rcs $t/b.a $t/a.o

cat <<EOF | $CC -c -o $t/c.o -xc -
#include <stdio.h>

extern char data[];
extern char __start_foo[];
extern char __stop_foo[];

int main() {
  printf("%.*s %s\n", (int)(__stop_foo - __start_foo), __start_foo, data);
}
EOF

$CC -B. -o $t/exe $t/c.o $t/b.a
$t/exe | grep -q 'section foo section foo'

$CC -B. -o $t/exe $t/c.o $t/b.a -Wl,-gc-sections
$t/exe | grep -q 'section foo section foo'

echo OK

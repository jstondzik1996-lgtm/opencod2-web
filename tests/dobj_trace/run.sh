#!/bin/sh
# Builds the REAL src/PC/xanim/dobj.c with this test as a freestanding 32-bit
# x86 Linux binary and runs DObjTraceline on a synthetic skeleton. Unused
# dobj.c dependencies stay unresolved (address 0); the test never calls them.
#   tests/dobj_trace/run.sh
#   DOBJ_SRC=/path/dobj.c tests/dobj_trace/run.sh
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${DOBJ_SRC:-$ROOT/src/PC/xanim/dobj.c}
OUT=${TMPDIR:-/tmp}/dobj_trace_$$
mkdir -p "$OUT/fake/gnu"
: > "$OUT/fake/gnu/stubs-32.h"
CFLAGS="--target=i686-linux-gnu -ffreestanding -fno-builtin -fno-pic -O1 -w -DDEDICATED \
 -I$ROOT/src/PC/speex -I$ROOT/src -I$ROOT/src/headers -I$ROOT -I$OUT/fake \
 -isystem /usr/include/x86_64-linux-gnu -isystem /usr/include"
clang $CFLAGS -c "$SRC" -o "$OUT/dobj.o"
clang $CFLAGS -c "$ROOT/tests/dobj_trace/test_dobj_trace.c" -o "$OUT/test.o"
clang --target=i686-linux-gnu -nostdlib -static -fno-pic -fuse-ld=ld \
  -Wl,--unresolved-symbols=ignore-all -Wl,--allow-multiple-definition \
  "$OUT/test.o" "$OUT/dobj.o" -o "$OUT/test"
timeout 20 "$OUT/test"

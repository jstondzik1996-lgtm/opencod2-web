#!/bin/sh
# Real src/PC/qcommon/cm_tracebox.c vs an exact segment/box test, i386.
#   tests/trace_box/run.sh  |  TRACEBOX_SRC=/path/cm_tracebox.c tests/trace_box/run.sh
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${TRACEBOX_SRC:-$ROOT/src/PC/qcommon/cm_tracebox.c}
OUT=${TMPDIR:-/tmp}/trace_box_$$
mkdir -p "$OUT/fake/gnu"
: > "$OUT/fake/gnu/stubs-32.h"
CFLAGS="--target=i686-linux-gnu -ffreestanding -fno-builtin -fno-pic -O1 -w -msse2 -mfpmath=sse -DDEDICATED \
 -I$ROOT/src/PC/speex -I$ROOT/src -I$ROOT/src/headers -I$ROOT -I$OUT/fake \
 -isystem /usr/include/x86_64-linux-gnu -isystem /usr/include"
clang $CFLAGS -c "$SRC" -o "$OUT/tb.o"
clang $CFLAGS -c "$ROOT/tests/trace_box/test_trace_box.c" -o "$OUT/test.o"
clang --target=i686-linux-gnu -nostdlib -static -fno-pic -fuse-ld=ld "$OUT/test.o" "$OUT/tb.o" -o "$OUT/test"
timeout 30 "$OUT/test"

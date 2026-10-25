#!/bin/sh
# Builds the real src/PC/qcommon/cm_world.c with stubs as a freestanding
# 32-bit x86 Linux binary and runs it. Needs clang + GNU ld (elf_i386) and a
# kernel that can execute i386 binaries. No game assets are used.
#   tests/cm_world_link/run.sh            -> test the current source
#   CM_WORLD_SRC=/path/cm_world.c run.sh  -> test another copy
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${CM_WORLD_SRC:-$ROOT/src/PC/qcommon/cm_world.c}
OUT=${TMPDIR:-/tmp}/cm_world_link_$$
mkdir -p "$OUT/fake/gnu"
: > "$OUT/fake/gnu/stubs-32.h"   # only glibc's 32-bit header shim is missing on x86_64 hosts
CFLAGS="--target=i686-linux-gnu -ffreestanding -fno-builtin -fno-pic -O1 -w -DDEDICATED \
 -I$ROOT/src/PC/speex -I$ROOT/src -I$ROOT/src/headers -I$ROOT -I$OUT/fake \
 -isystem /usr/include/x86_64-linux-gnu -isystem /usr/include"
clang $CFLAGS -c "$SRC" -o "$OUT/cm_world.o"
clang $CFLAGS -c "$ROOT/tests/cm_world_link/test_cm_world_link.c" -o "$OUT/test.o"
clang --target=i686-linux-gnu -nostdlib -static -fno-pic -fuse-ld=ld "$OUT/test.o" "$OUT/cm_world.o" -o "$OUT/test"
timeout 20 "$OUT/test"

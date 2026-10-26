#!/bin/sh
# Builds the REAL texture/vertex colour code as freestanding 32-bit x86 and
# checks the byte orders the WebGL path depends on (no game assets):
#   - src/PC/gfx_d3d/r_image_load_obj.c  IWI RGB24 / ARGB32 bitmap upload layout
#   - src/PC/gfx_d3d/r_utils.c           R_ConvertColorToBytes ("native" colour)
#   - src/Mac/DirectX_9/CColorConverter.c vertex colour converter
#   tests/texture_colors/run.sh
#   IMAGE_SRC=/path/r_image_load_obj.c tests/texture_colors/run.sh
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMG=${IMAGE_SRC:-$ROOT/src/PC/gfx_d3d/r_image_load_obj.c}
OUT=${TMPDIR:-/tmp}/texture_colors_$$
mkdir -p "$OUT/fake/gnu"
: > "$OUT/fake/gnu/stubs-32.h"
CFLAGS="--target=i686-linux-gnu -ffreestanding -fno-builtin -fno-pic -O1 -w -msse2 -mfpmath=sse \
 -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration \
 -I$ROOT/src/PC/speex -I$ROOT/src -I$ROOT/src/headers -I$ROOT -I$OUT/fake \
 -isystem /usr/include/x86_64-linux-gnu -isystem /usr/include"
clang $CFLAGS -c "$IMG" -o "$OUT/img.o"
clang $CFLAGS -c "$ROOT/src/PC/gfx_d3d/r_utils.c" -o "$OUT/utils.o"
clang $CFLAGS -c "$ROOT/src/Mac/DirectX_9/CColorConverter.c" -o "$OUT/conv.o"
clang $CFLAGS -c "$ROOT/tests/texture_colors/test_texture_colors.c" -o "$OUT/test.o"
clang --target=i686-linux-gnu -nostdlib -static -fno-pic -fuse-ld=ld \
  -Wl,--unresolved-symbols=ignore-all -Wl,--allow-multiple-definition \
  "$OUT/test.o" "$OUT/img.o" "$OUT/utils.o" "$OUT/conv.o" -o "$OUT/test"
timeout 20 "$OUT/test"

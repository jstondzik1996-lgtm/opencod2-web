#!/bin/sh
# Builds the REAL src/web/webgl2_compat.c against a fake WebGL2 context on the
# host (no Emscripten SDK, no game assets) and runs the regression checks.
#   tests/webgl2_compat/run.sh
#   COMPAT_SRC=/path/webgl2_compat.c tests/webgl2_compat/run.sh
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE="$ROOT/tests/webgl2_compat"
SRC=${COMPAT_SRC:-$ROOT/src/web/webgl2_compat.c}
OUT=${TMPDIR:-/tmp}/webgl2_compat_$$
CC=${CC:-cc}
mkdir -p "$OUT"
$CC -std=gnu99 -O1 -g -Wall -Wno-unused-function -D__EMSCRIPTEN__ \
  -I"$HERE/stub" -I"$HERE" -I"$ROOT/src/web" \
  "$SRC" "$HERE/fake_gl.c" "$HERE/test_webgl2_compat.c" -lm -o "$OUT/test"
timeout 60 "$OUT/test"

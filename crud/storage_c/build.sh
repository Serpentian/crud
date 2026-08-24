#!/bin/sh
# Prototype build of crud/storage_c.so against an uninstalled
# tarantool source tree (in-source build with generated
# src/module.h). Usage:
#   ./build.sh [path-to-tarantool-src]
set -e
TNT_DIR="${1:-$HOME/Programming/tnt/tarantool_clean}"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$SRC_DIR/.."
CC="${CC:-cc}"
$CC -shared -fPIC -fvisibility=hidden -std=gnu99 -Wall -Wextra -O2 -g \
    -I"$TNT_DIR/src" \
    -I"$TNT_DIR/src/lib/msgpuck" \
    -I"$TNT_DIR/third_party/luajit/src" \
    -o "$OUT_DIR/storage_c.so" \
    "$SRC_DIR/storage_c.c" \
    "$TNT_DIR/src/lib/msgpuck/libmsgpuck.a"
echo "built $OUT_DIR/storage_c.so"

#!/bin/sh
# Old/new benchmark of the crud simple DML operations.
#
#   run.sh [tarantool-binary] [old-revision]
#
# Runs the same bench (public crud API only) against three
# configurations and prints the tables one after another:
#
#   A. the old revision (default: the commit before this branch)
#      - the _crud.call_on_storage trampoline, fast mode
#   B. this revision, Lua storage functions (CRUD_C_DML unset)
#      x VSHARD_C_CALL 0/1
#   C. this revision, C storage functions (CRUD_C_DML=1)
#      x VSHARD_C_CALL 0/1
#
# The old revision is checked out into a temporary git worktree,
# so the working tree is left untouched.
set -e
TNT="${1:-tarantool}"
DIR="$(cd "$(dirname "$0")" && pwd)"
CRUD="$(cd "$DIR/../../.." && pwd)"
VSHARD="${VSHARD_DIR:-$HOME/Programming/tnt/vshard}"
WORK="${BENCH_WORK:-/tmp/crud_c_bench}"
OLD_REV="${2:-5b03736}"

echo "# tarantool: $($TNT --version | head -1)"
echo "# crud new: $(git -C "$CRUD" rev-parse --short HEAD), old: $(git -C "$CRUD" rev-parse --short "$OLD_REV")"

rm -rf "$WORK"
mkdir -p "$WORK/storage" "$WORK/router"

# The old revision lives in a worktree.
OLD_TREE="$WORK/crud_old"
git -C "$CRUD" worktree prune
git -C "$CRUD" worktree add -q --detach "$OLD_TREE" "$OLD_REV"
cleanup() {
    [ -f "$WORK/storage/storage.pid" ] &&
        kill "$(cat "$WORK/storage/storage.pid")" 2>/dev/null
    git -C "$CRUD" worktree remove --force "$OLD_TREE" 2>/dev/null
    return 0
}
trap cleanup EXIT

# $1 - crud dir, $2 - label, $3 - CRUD_C_DML, $4 - VSHARD_C_CALL
run_case() {
    crud_dir="$1"; label="$2"; c_dml="$3"; c_call="$4"
    rm -rf "$WORK/storage" "$WORK/router"
    mkdir -p "$WORK/storage" "$WORK/router"
    export LUA_PATH="$VSHARD/?.lua;$VSHARD/?/init.lua;$crud_dir/?.lua;$crud_dir/?/init.lua;$CRUD/.rocks/share/tarantool/?.lua;$CRUD/.rocks/share/tarantool/?/init.lua;;"
    export LUA_CPATH="$crud_dir/?.so;$VSHARD/?.so;$CRUD/.rocks/lib/tarantool/?.so;;"
    export CRUD_C_DML="$c_dml"
    export VSHARD_C_CALL="$c_call"
    (cd "$WORK/storage" && "$TNT" "$DIR/storage.lua" > storage.log 2>&1 &)
    i=0
    while [ ! -f "$WORK/storage/storage.pid" ] && [ $i -lt 120 ]; do
        sleep 0.5
        i=$((i + 1))
    done
    if [ ! -f "$WORK/storage/storage.pid" ]; then
        echo "storage failed to start ($label):" >&2
        tail -20 "$WORK/storage/storage.log" >&2
        return 1
    fi
    (cd "$WORK/router" && BENCH_LABEL="$label" "$TNT" "$DIR/bench.lua" \
        2>/dev/null)
    kill "$(cat "$WORK/storage/storage.pid")" 2>/dev/null || true
    sleep 1
    echo
}

run_case "$OLD_TREE" "A old (trampoline, fast mode)"      "" ""
run_case "$CRUD"     "B new Lua funcs, vshard Lua"        "" ""
run_case "$CRUD"     "B new Lua funcs, vshard C"          "" "1"
run_case "$CRUD"     "C new C funcs, vshard Lua"          "1" ""
run_case "$CRUD"     "C new C funcs, vshard C"            "1" "1"

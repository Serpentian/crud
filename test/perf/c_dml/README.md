# How to run the simple-DML old/new benchmark

Measures the public crud API (`get`, `replace`, `update`,
`insert`+`delete`) through a router against a single storage, and
compares the revision before this branch with the current one — with
the Lua and with the C storage functions, and with the Lua and the C
vshard `storage.call` wrapper.

## 1. Prerequisites

**A patched tarantool.** The C path needs `box_func_call_by_name()` in
the module API (a core patch). Check the binary you are going to run:

```sh
nm -D <tarantool-binary> | grep box_func_call_by_name
```

Use a release build — Debug numbers are meaningless.

**The vshard prototype.** The bench uses the vshard checkout with the C
`storage.call`, not the vshard rock from `.rocks`. Point `VSHARD_DIR`
at it (default `$HOME/Programming/tnt/vshard`) and build its module
once:

```sh
$VSHARD_DIR/vshard/storage_c/build.sh [path-to-tarantool-src]
```

**The crud module.** Build `crud/storage_c.so` in *this* checkout:

```sh
crud/storage_c/build.sh [path-to-tarantool-src]
```

Both build scripts default to `$HOME/Programming/tnt/tarantool_clean`;
that path is used for headers and for linking `libmsgpuck.a` only, so
it need not match the binary being run.

> The old revision is checked out into a temporary git worktree, which
> does not contain `storage_c.so` — that is fine, the old revision has
> no C path at all.

## 2. Run

```sh
test/perf/c_dml/run.sh <tarantool-binary> [old-revision]
```

`old-revision` defaults to `5b03736` (the commit this branch starts
from). The script prints five tables:

| label | what it measures |
|---|---|
| A old (trampoline, fast mode) | the old revision: `_crud.call_on_storage`, no bucket refs |
| B new Lua funcs, vshard Lua | this revision through `vshard.router.call`, Lua storage functions |
| B new Lua funcs, vshard C | same, with the C vshard wrapper |
| C new C funcs, vshard Lua | `CRUD_C_DML=1`, C storage functions, Lua vshard wrapper |
| C new C funcs, vshard C | the fully-C path |

Environment:

| variable | default | meaning |
|---|---|---|
| `VSHARD_DIR` | `$HOME/Programming/tnt/vshard` | vshard checkout with the C prototype |
| `BENCH_FIBERS` | 10 | concurrent request fibers |
| `BENCH_DURATION` | 10 | measured seconds per cell |
| `BENCH_WARMUP` | 2 | warmup seconds before measuring |
| `BENCH_WORK` | `/tmp/crud_c_bench` | working directory of the instances and of the worktree |

Example:

```sh
BENCH_DURATION=20 BENCH_FIBERS=20 \
    test/perf/c_dml/run.sh ~/Programming/tnt/tarantool/src/tarantool
```

## 3. Reading the output

Columns: RPS, then p50/p95/p99 in microseconds, then the error count
(must be 0). `insert+delete` performs two requests per iteration and is
WAL-bound, so it is the noisiest row.

Variance between runs on a loaded machine reaches tens of percent —
compare labels within one run, not across runs, and repeat before
trusting a small delta. Measured results and their interpretation are
in `RESULTS.md`.

## 4. Cleanup

`run.sh` kills the storage and removes the worktree on exit. After an
interrupted run:

```sh
pkill -f 'c_dml/storage.lua'
git worktree prune
rm -rf /tmp/crud_c_bench
```

## 5. Related: the functional tests

The same environment runs the luatest suites; note that the
`.rocks/bin/luatest` launcher hardcodes another tarantool binary and
puts `.rocks` first in the path, which would shadow the vshard
prototype with the vshard rock, so call the inner script directly:

```sh
CRUD=$PWD; VSHARD=${VSHARD_DIR:-$HOME/Programming/tnt/vshard}
export LUA_PATH="$VSHARD/?.lua;$VSHARD/?/init.lua;$CRUD/?.lua;$CRUD/?/init.lua;;"
export LUA_CPATH="$CRUD/?.so;$VSHARD/?.so;;"
CRUD_C_DML=1 VSHARD_C_CALL=1 <tarantool-binary> \
    -e 'package.path=os.getenv("LUA_PATH")..".rocks/share/tarantool/?.lua;.rocks/share/tarantool/?/init.lua;"..package.path;
        package.cpath=os.getenv("LUA_CPATH")..package.cpath' \
    .rocks/share/tarantool/rocks/luatest/1.0.1-1/bin/luatest \
    test/integration/simple_operations_test.lua test/integration/storage_c_test.lua
```

Drop `CRUD_C_DML` / `VSHARD_C_CALL` to exercise the Lua paths.

# crud simple DML: old vs new, benchmark results

Date: 2026-08-24. Local dev box, 16 cores. Tarantool
3.9.0-entrypoint-86 RelWithDebInfo (with the `box_func_call_by_name`
patch), our vshard from `tnt/vshard`, one storage process + an
in-process router, 10 fibers, 8 s per cell after a 2 s warmup.
Reproduce: `test/perf/c_dml/run.sh <tarantool-binary>`.

Configurations:

- **A old** — `5b03736` (before this branch): the
  `_crud.call_on_storage` trampoline, fast mode (no bucket refs at all).
- **B new, Lua funcs** — this branch with `CRUD_C_DML` unset: requests
  go through `vshard.router.call`, storage functions are still Lua.
- **C new, C funcs** — `CRUD_C_DML=1`: the C storage functions.

B and C are each measured with the vshard wrapper in Lua
(`VSHARD_C_CALL` unset) and in C (`VSHARD_C_CALL=1`).

## RPS (two runs; the second one shown, the first is in the git history of this file)

| workload      | A old | B Lua/Lua | B Lua/C | C C/Lua | **C C/C** |
|---------------|-------|-----------|---------|---------|-----------|
| replace       | 35462 | 46740     | 36587   | 24606   | **41536** |
| get           | 82924 | 41910     | 48490   | 72475   | **99069** |
| update        | 40339 | 47073     | 63356   | 28205   | **77950** |
| insert+delete | 14009 | 23911     | 14875   | 19880   | **16121** |

p50 latency, fully-C column vs old: get 82.8 vs 99.4 µs, update 108.7
vs 210.4 µs, replace 211.4 vs 249.0 µs.

## Reading the numbers

- The fully-C path (C storage functions + C vshard wrapper) is the best
  cell for the single-tuple reads and updates: get ~99k RPS vs ~83k for
  the old trampoline+fast-mode path (+19%), update ~78k vs ~40k (+93%),
  and that is *with* real bucket references now being taken on every
  request, which the old fast mode skipped entirely.
- The C storage functions only pay off together with the C vshard
  wrapper: the "C funcs / Lua wrapper" column is consistently worse,
  because the Lua wrapper's `netbox.self:call` marshalling dominates and
  the C function's own savings are small next to it.
- `insert+delete` does two round trips per iteration and is dominated by
  WAL writes, so it moves the least and is the noisiest cell.
- Run-to-run variance on this shared box is large (the first run had
  old-get at 39k against 83k here). Compare columns within one run
  only; the stable signal is the C/C column leading on get and update.

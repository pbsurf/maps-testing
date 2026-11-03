# Ascend Maps testing

Tests and benchmarks for Ascend Maps (repo `styluslabs/maps-testing`, a submodule of `explore` at
`testing/`). Everything here is built and run from the `explore` root.

| folder | what | run |
|---|---|---|
| `bench/` | headless benchmark replay of the app's scene over a fixed tile fixture: callgrind, memory and counters per phase (see `bench/README.md`) | `make -f testing/bench.mk`, then `testing/bench/bench.py` |
| `plugins/` | tests for the JS plugins in `assets/plugins`, run with duktape; optional live API data via `fetch-data.sh` (not committed) | `make -C testing/plugins` |

## What lives where

This repo is for test and benchmark code, which can grow quickly. Changes to the application source
(`app/`, `tangram-es/core/`) are kept small; hooks the tests need there are limited to:
- `tangram-es/core/src/debug/benchCounters.h` and its `BENCH_COUNT` calls (compiled in only with
  `TANGRAM_BENCH`)
- small accessors: `LabelManager::stats()` and `dumpLabels()` (`TANGRAM_BENCH` only),
  `TileManager::tasksInFlight()`, raster cache stats in `RasterSource`

Tangram's own unit tests stay in `tangram-es/tests/` (run with `make -f tests.mk` from the `explore`
root, as in CI), so the tangram fork keeps its tests. The benchmark also uses the null GL driver from
there, `tangram-es/tests/src/gl_mock.cpp`.

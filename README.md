# Ascend Maps testing

Tests and benchmarks for Ascend Maps (repo `pbsurf/maps-testing`, a submodule of `styluslabs/maps` at
`testing/`). Everything here is built and run from the root of the `styluslabs/maps` checkout.

| folder | what | run |
|---|---|---|
| `bench/` | headless benchmark replay of the app's scene over a fixed tile fixture: callgrind (with counters) and memory per phase (see `bench/README.md`) | `make -f testing/bench.mk`, then `testing/bench/bench.py` |
| `plugins/` | tests for the JS plugins in `assets/plugins`, run with duktape; optional live API data via `fetch-data.sh` (not committed) | `make -C testing/plugins` |

## What lives where

This repo is for test and benchmark code, which can grow quickly. Changes to the application source
(`app/`, `tangram-es/core/`) are kept small, with no benchmark-only code; the replay uses general
accessors: `TileManager::tasksInFlight()`, `LabelManager::numLabels()` and
`RasterSource::cacheMemoryUsage()`. Counters come from callgrind call counts, and diagnostics and A/B
switches are patches in `bench/patches/`, applied to the tangram working tree only when needed.

Tangram's own unit tests stay in `tangram-es/tests/` (run with `make -f tests.mk` from the
`styluslabs/maps` root, as in CI), so the tangram fork keeps its tests. The benchmark also uses the null GL driver from
there, `tangram-es/tests/src/gl_mock.cpp`.

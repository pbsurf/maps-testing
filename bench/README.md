# Tangram benchmark replay

Headless replay of the app's real scene over a fixed tile fixture, for catching CPU and memory
regressions. The plan is in `~/maps/PROJECTS.md` ("Tangram benchmark harness"). It replaces the upstream
Google Benchmark suite in `tangram-es/bench/`, which is no longer used.

## Build

From the `explore` root (all commands below assume it):

    make -f testing/bench.mk -j2        # -> build/Bench/replay

`testing/bench.mk` builds tangram core with `TANGRAM_BENCH` (enables `core/src/debug/benchCounters.h`) and
`PROFILE=1` (frame pointers, no AVX-512, for valgrind and perf), and links:
- `tangram-es/tests/src/gl_mock.cpp`: null GL driver that returns real object ids and tracks GPU memory
- the app's tangram user functions (`app/src/stylefns.cpp`, `app/src/tangramfns.cpp`) with usvg, ulib
  and nanovg (single-header implementations in `benchimpl.cpp`), so native style functions, SDF glyphs and SVG sprites cost the same as in the app

## Fixture

Tiles come from tiles.styluslabs.com (vector, source `osm`) and the ArcGIS elevation service (source
`elevation`), cached in one mbtiles file per source in the fixture directory.

    testing/bench/bench.py fetch --fixture ~/maps/bench-fixture

This plays the script online with the disk cache pointed at the fixture (logging every request with
its phase to `requests.tsv`), deletes the z >= 14 elevation tiles used only by the `missing_rasters`
phase (`prune`), verifies that an offline run finds every other tile, and writes `fixture.json` with
the **fixture ID**: a hash of tile IDs and data (not of the files, which also hold timestamps). If
downloads fail, rerun with `--resume`. `bench.py id --fixture DIR` recomputes the ID.

Only one fixture is current. When the script changes, or the servers change their tiles, fetch a new
fixture into a new directory and compare its ID; if it differs, re-run the base commit on it before
comparing anything.

Elevation tiles keep their scene `max_age` (2 years); vector tiles never expire in the replay.

## Runs

    testing/bench/bench.py callgrind --fixture ~/maps/bench-fixture --out OUTDIR
    testing/bench/bench.py memory --fixture ~/maps/bench-fixture --out OUTDIR [--runs 3]

`callgrind` runs the replay under `valgrind --tool=callgrind`; each phase is zeroed and dumped
separately (`CALLGRIND_DUMP_STATS_AT("<phase>")`), giving one file per phase (`callgrind.out.N`; the
last file is the dump at exit) covering all threads. `summary.json` has instruction totals per phase:
`total_ir` (all threads), `main_ir` (the main thread's per-frame work: inclusive cost of the
`bench_main_*` functions for update, render and scene load) and `tile_worker_ir` (inclusive cost of
`TileWorker::run`). The remainder is mbtiles and other threads. Open the files with `callgrind_annotate` or kcachegrind.

Repeatability (two identical runs of startup + pan): all totals agree within 0.01%. Callgrind runs
threads one at a time, so it hides thread races; use `memory` runs (exact counters) to check
determinism. A startup + pan run takes about 2.5 minutes, all phases about 20 minutes; use
`--stop-after pan` while iterating. Runs can't share a fixture (the replay writes tile access times to it);
`bench.py` locks the fixture and refuses a second concurrent run.

`memory` runs natively with `MALLOC_ARENA_MAX=1` and reports per phase: live heap and arena
(`mallinfo2`), peak RSS (`VmHWM`, reset at phase start via `/proc/self/clear_refs`), GPU texture,
buffer and renderbuffer bytes from `gl_mock`, upload counts, tile cache and raster cache sizes, and
the counters below. Timing and memory fields are medians over the runs; other fields that differ
between runs are listed as `nondeterministic`.

Both pass `--workers 1` by default (one tile worker thread); use `--workers N` to change it. Use
`--phases a,b` to measure only some phases (the others still run, unmeasured, so state is the same)
and `--stop-after PHASE` for quick iterations.

The replay can also be run directly: `build/Bench/replay --help`. `BENCH_VERBOSE=1` logs the state of
every frame spent waiting for tiles.

## Phases (Paris)

| phase | what |
|---|---|
| `startup` | scene load centered on Paris (48.857, 2.352) at z14 |
| `pan` | pans across central Paris at z15.5 and z16 (dense labels: `handleOcclusions`) |
| `zoom` | zoom out to z10, back in to z17 |
| `idle` | 300 steady-state frames at z16, nothing loading |
| `terrain_load` | scene reload in 3D terrain mode |
| `terrain` | rotate and tilt in terrain mode (Paris is flat: measures terrain overhead) |
| `missing_rasters` | zoom into Bois de Vincennes, whose z >= 14 elevation tiles are pruned from the fixture, so tiles use proxy rasters (`1c74a23c`) |

Setup steps for a phase (camera jumps, waiting for tiles) aren't measured.

## Determinism

With `--workers 1`, native runs give identical results apart from timing, heap and RSS: label counts,
collision candidates, buffer and texture bytes, uploads and draw calls all match exactly (checked over 5
runs of all phases). `bench.py memory` lists any field that differs between runs as `nondeterministic`,
so a new entry there means a new source of nondeterminism.

What makes this work:
- Each frame waits until no tile task is being fetched or built (`TileManager::tasksInFlight()`), both
  before `update()` and between `update()` and `render()`. Tasks dispatched by `update()` would otherwise
  add glyphs to the shared glyph atlas while `render()` uploads it. Tile results are only taken by the
  next `update()`, so the extra wait doesn't change what a frame shows.
- It also waits for the offscreen terrain worker and for `Map`'s async worker, which deletes the old
  scene after `loadScene`.
- Label ordering doesn't use memory addresses (tangram commit `a1504b90`): ties in
  `LabelManager::priorityComparator` and `zOrderComparator` are broken by the label's position in
  `m_labels` (tiles, styles and markers are visited in a fixed order), style ID and texture creation
  serial. The default repeat group (`DrawRule::getParamSetHash()`) hashes layer names, not pointers to
  them.

Waiting is excluded from callgrind's main-thread cost (it's outside the `bench_main_*` functions).

Debugging tools for finding where two runs diverge (diff the files from two runs):
- `BENCH_DUMP_LABELS=FILE`: every label entry after every frame (tile, style, seq, state, hash, screen
  position)
- `BENCH_DUMP_TEX=FILE`: every texture upload with its size and caller

## Counters

From `core/src/debug/benchCounters.h` (only with `TANGRAM_BENCH`) and `LabelManager::stats()`:
- `collision_candidates`: isect2d hits tested against OBBs in `handleOcclusions`
- `proxy_raster_lookups`: raster texture lookups for tiles without their exact raster
- `tile_requests`, `tile_builds`, `network_requests` (offline: tiles missing from the fixture)
- `labels_peak`, `obbs_peak`, `obb_labels_capacity`

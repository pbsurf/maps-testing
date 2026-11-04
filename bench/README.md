# Tangram benchmark replay

Headless replay of the app's real scene over a fixed tile fixture, for catching CPU and memory
regressions. It replaces the upstream Google Benchmark suite in `tangram-es/bench/`, which is no longer
used.

## Build

From the root of the `styluslabs/maps` checkout (all commands below assume it):

    make -f testing/bench.mk -j2        # -> build/Bench/replay

`testing/bench.mk` builds tangram core unchanged, with `PROFILE=1` (frame pointers, no AVX-512, for
valgrind and perf), and links:
- `tangram-es/tests/src/gl_mock.cpp`: null GL driver that returns real object ids and tracks GPU memory
- the app's tangram user functions (`app/src/stylefns.cpp`, `app/src/tangramfns.cpp`) with usvg, ulib
  and nanovg (single-header implementations in `benchimpl.cpp`), so native style functions, SDF glyphs and SVG sprites cost the same as in the app

## Fixture

Tiles come from tiles.styluslabs.com (vector, source `osm`) and the ArcGIS elevation service (source
`elevation`), cached in one mbtiles file per source in the fixture directory (`FIXTURE` below; keep it
outside the repo).

    testing/bench/bench.py fetch --fixture FIXTURE

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

    testing/bench/bench.py callgrind --fixture FIXTURE --out OUTDIR
    testing/bench/bench.py memory --fixture FIXTURE --out OUTDIR [--runs 3]

`callgrind` runs the replay under `valgrind --tool=callgrind`; each phase is zeroed and dumped
separately (`CALLGRIND_DUMP_STATS_AT("<phase>")`), giving one file per phase (`callgrind.out.N`; the
last file is the dump at exit) covering all threads. `summary.json` has instruction totals per phase:
`total_ir` (all threads), `main_ir` (the main thread's per-frame work: inclusive cost of the
`bench_main_*` functions for update, render and scene load) and `tile_worker_ir` (inclusive cost of
`TileWorker::run`). The remainder is mbtiles and other threads. It also has the counters below, taken
from the dumps' call counts. Open the files with `callgrind_annotate` or kcachegrind.

Repeatability (two identical runs of startup + pan): all totals agree within 0.01%. Callgrind runs
threads one at a time, so it hides thread races; use `memory` runs (exact GPU and label fields) to
check determinism. Use `--phases` or `--stop-after` while iterating. Runs can't share a fixture (the replay writes tile access times to it);
`bench.py` locks the fixture and refuses a second concurrent run.

`memory` runs natively with `MALLOC_ARENA_MAX=1` and reports per phase: live heap and arena
(`mallinfo2`), peak RSS (`VmHWM`, reset at phase start via `/proc/self/clear_refs`), GPU texture,
buffer and renderbuffer bytes from `gl_mock`, upload and draw call counts, tile cache and raster cache
sizes, peak label count and `network_requests` (offline: tiles missing from the fixture). Timing and
memory fields are medians over the runs; other fields that differ
between runs are listed as `nondeterministic`.

Both pass `--workers 1` by default (one tile worker thread); use `--workers N` to change it. Use
`--phases a,b` to run and measure only some phases: the others are skipped, except the scene loads
(`startup`, `terrain_load`) that later phases need. Each phase's setup moves the camera where it needs
it, but tile caches start colder than in a full run, so only compare runs with the same `--phases`
(recorded in the results header). `--stop-after PHASE` stops after a phase.

The results header records `--phases` and any `BENCH_*` environment variables (`bench_env`), such as the
switches added by the patches below.

The replay can also be run directly: `build/Bench/replay --help`. `BENCH_VERBOSE=1` logs the state of
every frame spent waiting for tiles.

## Phases (Paris)

| phase | what |
|---|---|
| `startup` | scene load centered on Paris (48.857, 2.352) at z14 |
| `pan` | pans one view width (3.5 km) along the Seine at z15.5 in 30 frames, Concorde to the Marais (dense labels: `handleOcclusions`); the start view loads in setup |
| `idle` | 60 steady-state frames where pan ended, nothing loading |
| `zoom` | zoom out from z17 to z10 north of Paris, 3 frames per tile zoom level |
| `terrain_load` | scene reload in 3D terrain mode |
| `terrain` | tilt to 60° in terrain mode over 10 frames (Paris is flat: measures terrain overhead) |
| `missing_rasters` | about 18 km east of Paris, whose z >= 14 elevation tiles are pruned from the fixture: jump from z12.5 to z15, so tiles use proxy rasters, then 10 frames on proxies (`cccd4277`) |

Setup steps for a phase (camera jumps, waiting for tiles) aren't measured.

The script spends frames only where they add work:
- Each frame waits for its tiles, so every frame's view is fully built, and the tile cache only holds
  about one screen of tiles. The script never returns to an area at a tile zoom level it has already
  built, which would measure the same builds again. No phase rotates the view: in a tilted view that
  makes tiles switch between zoom levels, and they get rebuilt.
- `zoom` steps through each level at L+0.9, L+0.7 and L+0.5 instead of zooming continuously: a
  continuous zoom passes L+0.0, whose view has about twice the tiles of L+0.5.
- Waiting for a view to complete uses 0.25 s frames: labels fade out over 1 s, which would otherwise
  take 60 nearly identical frames.

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
- Label ordering doesn't use memory addresses (tangram commit `8053e642`): ties in
  `LabelManager::priorityComparator` and `zOrderComparator` are broken by the label's position in
  `m_labels` (tiles, styles and markers are visited in a fixed order), style ID and texture creation
  serial. The default repeat group (`DrawRule::getParamSetHash()`) hashes layer names, not pointers to
  them.

Waiting is excluded from callgrind's main-thread cost (it's outside the `bench_main_*` functions).

Debugging tools for finding where two runs diverge (diff the files from two runs):
- `BENCH_DUMP_TEX=FILE`: every texture upload with its size and caller (in `gl_mock`, always available)
- `BENCH_DUMP_LABELS=FILE`: with `patches/dump-labels.patch`, see below

## Counters (callgrind)

`bench.py callgrind` takes these from the call counts in each phase's dump (`CALL_COUNTS` in
`bench.py`), so tangram needs no counter code:
- `tile_builds`: `TileBuilder::build` calls
- `tile_requests`: `loadTileData` calls from `TileManager::loadTiles`
- `collision_candidates`: calls of the isect2d callback in `LabelManager::handleOcclusions` (AABB hits
  tested against OBBs)
- `proxy_raster_lookups`: `RasterSource::getTexture` calls from `TileManager::updateTileSet` (raster
  lookups for tiles without their exact raster)

## Patches

Tangram has no benchmark-only code. Diagnostics and A/B switches are patches in `patches/`, applied to
the tangram working tree when needed and reverted afterwards, so they never get committed:

    git -C tangram-es apply ../testing/bench/patches/NAME.patch     # then make -f testing/bench.mk
    git -C tangram-es apply -R ../testing/bench/patches/NAME.patch  # when done

Each patch is inactive unless its environment variable is set (recorded in the results header as
`bench_env`), and the switches log a warning when active, so one patched binary runs both sides of an
A/B:
- `old-obb-lookup.patch`: `BENCH_OLD_OBB_LOOKUP=1` uses the linear OBB to label scan from before
  tangram `2457aa25`
- `old-proxy-raster.patch`: `BENCH_OLD_PROXY_RASTER=1` keeps proxy rasters as before `cccd4277`
- `dump-labels.patch`: `BENCH_DUMP_LABELS=FILE` writes every label entry (tile, source, style, index in
  draw order, state, priority, hash, screen position) after each label update, with tangram's frame
  number; diff the files from two runs to find where they diverge
- `build-cpu-log.patch`: `BENCH_BUILD_LOG=FILE` writes one line per tile task: source, tile, thread CPU
  microseconds, and 1 if it built a tile (native runs: per-tile cost by source and zoom)

For a new change, add a switch the same way: a `static const bool` from `getenv("BENCH_OLD_...")`
guarding the old code path. Patches can stop applying as tangram changes; refresh them with
`git diff > patches/NAME.patch` after fixing them up by hand.

## Reference numbers

Fixture `2d6050aafdf130c8` (165 vector and 269 elevation tiles, 36 elevation tiles pruned for
`missing_rasters`), `--workers 1`, all phases, on the dev VM (2 vCPUs, i7-1165G7 host), 2026-11-04:
- Measured phases: 161 frames and 501 tile builds. pan 35 frames / 26 builds, zoom 30 / 261, terrain
  17 / 74, missing_rasters 13 / 8.
- Callgrind: about 6 minutes for a full run, 53.6G Ir in total; startup takes about 1 minute and zoom
  2.3 minutes; `--phases missing_rasters` alone takes under 2 minutes.
- Native: main thread about 0.47 s, tile worker about 6.1 s, of which zoom's 110 mvt builds take about
  2.1 s and startup's 40 about 1.4 s. Tile-worker CPU per build (`build-cpu-log.patch`): mvt 11-80 ms
  depending on density (central Paris 70-80 ms), elevation about 0.3 ms (LERC decode), so elevation is
  about 2% of tile-worker time despite more builds.

A rare nondeterminism was seen once: 1 native run in about 50 had one GPU buffer fewer at startup (631
vs 632), while a callgrind run was starting alongside. Not reproduced since; if it shows up again, diff
`BENCH_DUMP_TEX` output from the two runs.

## Possible extensions (not started)

- **Results log and compare script.** An append-only `results.jsonl` (or SQLite) outside every repo, one
  record per run: tangram commit plus a hash of uncommitted changes, fixture ID, phases, `bench_env`,
  per-phase totals, top functions by inclusive and self cost, memory fields, counters, host and date. The
  compare script pairs records with the same fixture ID and phases and prints per-phase deltas. It
  compares deterministic fields exactly, and RSS and heap as a median of 3 runs, flagged above about 3-5%.
  If the current fixture has no result for the base commit, it says so. Optional call graph via
  `gprof2dot`. Today, comparing means diffing `summary.json` from two output directories.
- **Automatic red flags:** functions whose calls per frame grow over `idle`; a DHAT pass on `idle` and
  `pan` listing allocating call sites in per-frame paths; dense vs sparse views to catch costs that grow
  much faster than tile or label count; tiles built twice, repeated requests; heap or GPU bytes rising
  over `idle`.
- **Loop test:** play the pan path, return to the start and play it again; heap and GPU bytes after the
  second loop should match the first, within cache limits (more useful than LSan, since most memory is
  freed at shutdown).
- **Heap profiling:** Massif with per-phase snapshots via
  `VALGRIND_MONITOR_COMMAND("detailed_snapshot <phase>.massif")`, the same pattern as the callgrind phase
  dumps; heaptrack is faster but can't split by phase. More memory fields: `sqlite3_memory_used()` (mbtiles
  cache) and the font atlas size.
- **General A/B:** two persistent git worktrees (base and test) with their own build directories, so each
  comparison only recompiles changed files; ccache (`CCACHE_BASEDIR`, plus `CCACHE_NOHASHDIR` for `-g`
  builds) and mold/lld would speed this up. For small focused changes, prefer a patch with a runtime
  switch (see Patches): no rebuild, and no noise from code layout.
- **perf:** callgrind's instruction counts are the repeatable signal, but its cache model is simplistic and
  it runs one thread at a time. `perf stat` / `perf record` modes for the replay and a flame-graph script
  for the live app would confirm real-time gains and find cache, thread or driver costs. Hardware
  counters aren't available in the dev VM yet; timer sampling works.
- **Single-tile microbenchmark:** build one fixed tile from the fixture with the app's scene and split the
  cost by stage (MVT parse, style matching and JS functions, geometry building); quicker and more focused
  under callgrind than the replay. The removed upstream Google Benchmark suite had this shape. More
  generally, microbenchmarks only for hotspots the replay finds (for example `handleOcclusions` at
  increasing label counts): nanobench if hardware counters work, otherwise Google Benchmark plus callgrind.

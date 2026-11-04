#!/usr/bin/env python3
"""Driver for the tangram benchmark replay (see README.md). Run from the explore repo root.

  bench.py fetch --fixture DIR      download a new fixture, prune it, verify offline, write its ID
  bench.py id --fixture DIR         print fixture ID (hash of tile IDs and data, not of the files)
  bench.py prune --fixture DIR      delete elevation tiles for the missing_rasters phase
  bench.py callgrind --fixture DIR --out OUTDIR   per-phase callgrind profiles + summary.json
  bench.py memory --fixture DIR --out OUTDIR      native runs (memory, counters) + summary.json
"""

import argparse
import fcntl
import glob
import hashlib
import json
import os
import re
import sqlite3
import statistics
import subprocess
import sys
import time

REPLAY = "build/Bench/replay"
# elevation source url from assets/scenes/elevation.yaml
ELEVATION_URL = ("https://elevation3d.arcgis.com/arcgis/rest/services/WorldElevation3D/Terrain3D/"
                 "ImageServer/tile/{z}/{y}/{x}")
PRUNE_PHASE = "missing_rasters"
PRUNE_MIN_ZOOM = 14
# phases expected to request missing tiles in offline runs
MISSING_OK_PHASES = {PRUNE_PHASE}
# memory fields reported as median over runs (RSS and heap are slightly noisy); others must match exactly
NOISY_FIELDS = {"wall_s", "main_cpu_s", "process_cpu_s", "heap_live", "heap_arena", "heap_mmap",
                "rss", "rss_peak"}


def die(msg):
    sys.exit("bench.py: " + msg)


WORKERS = "1"  # set from --workers


def run_replay(fixture, extra, out_json, env=None, wrapper=None):
    if not os.path.exists(REPLAY):
        die(f"{REPLAY} not found; build with: make -f testing/bench.mk (from the explore repo root)")
    cmd = (wrapper or []) + [REPLAY, "--fixture", fixture, "--out", out_json, "--workers", WORKERS] + extra
    print("+", " ".join(cmd), file=sys.stderr)
    # the replay writes tile access times to the fixture's mbtiles files, so concurrent runs on one
    #  fixture fail with "database is locked"
    with open(os.path.join(fixture, ".lock"), "w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            die(f"another bench run is using {fixture}")
        res = subprocess.run(cmd, env=env)
    with open(out_json) as f:
        return res.returncode, json.load(f)


def fixture_id(fixture):
    """Hash of tile IDs and data in a fixed order; download timestamps and access times are excluded."""
    h = hashlib.sha256()
    counts = {}
    for path in sorted(glob.glob(os.path.join(fixture, "*.mbtiles"))):
        name = os.path.basename(path)
        h.update(name.encode() + b"\0")
        db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        n = 0
        for z, x, y, data in db.execute("SELECT zoom_level, tile_column, tile_row, tile_data FROM tiles "
                                        "ORDER BY zoom_level, tile_column, tile_row"):
            h.update(f"{z}/{x}/{y}:{len(data or b'')}\0".encode())
            h.update(data or b"")
            n += 1
        db.close()
        counts[name] = n
    return h.hexdigest()[:16], counts


def template_regex(url):
    pat = re.escape(url)
    for k in "zxy":
        pat = pat.replace(re.escape("{%s}" % k), f"(?P<{k}>\\d+)")
    pat = pat.replace(re.escape("{s}"), "[^/]+")
    return re.compile("^" + pat + "(\\?.*)?$")


def cmd_prune(args):
    log = os.path.join(args.fixture, "requests.tsv")
    db_path = os.path.join(args.fixture, "elevation.mbtiles")
    if not os.path.exists(log) or not os.path.exists(db_path):
        die(f"need {log} and {db_path} from a fetch run")
    rx = template_regex(ELEVATION_URL)
    by_phase = {}
    with open(log) as f:
        for line in f:
            phase, _, url = line.rstrip("\n").partition("\t")
            m = rx.match(url)
            if m:
                by_phase.setdefault(phase, set()).add((int(m["z"]), int(m["x"]), int(m["y"])))
    target = {t for t in by_phase.get(PRUNE_PHASE, set()) if t[0] >= PRUNE_MIN_ZOOM}
    others = set().union(*[v for k, v in by_phase.items() if k != PRUNE_PHASE]) if by_phase else set()
    target -= others
    db = sqlite3.connect(db_path)
    deleted = 0
    for z, x, y in sorted(target):
        tms_y = (1 << z) - 1 - y
        cur = db.execute("DELETE FROM images WHERE tile_id IN (SELECT tile_id FROM map WHERE zoom_level = ? "
                         "AND tile_column = ? AND tile_row = ?)", (z, x, tms_y))
        deleted += cur.rowcount
    db.commit()
    db.execute("VACUUM")
    db.close()
    print(f"pruned {deleted} of {len(target)} elevation tiles (z >= {PRUNE_MIN_ZOOM}) used only by "
          f"phase {PRUNE_PHASE}")


def cmd_id(args):
    fid, counts = fixture_id(args.fixture)
    print(fid)
    for name, n in counts.items():
        print(f"  {name}: {n} tiles", file=sys.stderr)
    return fid, counts


def offline_check(results):
    """Return list of (phase, misses) for phases that requested tiles missing from the fixture."""
    return [(p["phase"], p["network_requests"]) for p in results["phases"]
            if p["network_requests"] > 0 and p["phase"] not in MISSING_OK_PHASES]


def cmd_fetch(args):
    os.makedirs(args.fixture, exist_ok=True)
    if glob.glob(os.path.join(args.fixture, "*.mbtiles")) and not args.resume:
        die(f"{args.fixture} already has tiles; use --resume to retry failed downloads, or a new directory")
    rc, res = run_replay(args.fixture, ["--fetch"], os.path.join(args.fixture, "fetch.json"))
    if rc != 0:
        die("fetch run failed (see log above); run again with --resume to retry")
    cmd_prune(args)
    rc, res = run_replay(args.fixture, [], os.path.join(args.fixture, "verify.json"))
    misses = offline_check(res)
    if rc != 0 or misses:
        die(f"offline verification failed: {misses or 'replay error'}; run fetch --resume")
    fid, counts = fixture_id(args.fixture)
    info = {"fixture_id": fid, "created": time.strftime("%Y-%m-%d %H:%M:%S"), "tiles": counts}
    with open(os.path.join(args.fixture, "fixture.json"), "w") as f:
        json.dump(info, f, indent=2)
    print(f"fixture {fid}: {counts}")


def parse_callgrind(path):
    """Trigger (phase), total Ir, and inclusive Ir of the functions in COST_GROUPS for one dump."""
    info = {"file": os.path.basename(path), "trigger": "", "ir": 0}
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("desc: Trigger:"):
                info["trigger"] = line.split("Trigger:", 1)[1].strip()
            elif line.startswith("summary:") or line.startswith("totals:"):
                info["ir"] = int(line.split()[1])
    out = subprocess.run(["callgrind_annotate", "--inclusive=yes", "--threshold=100", path],
                         capture_output=True, text=True).stdout
    for group, pattern in COST_GROUPS.items():
        rx = re.compile(pattern)
        # code inlined from other files shows up as extra lines for the same function, so take the max
        #  per function (the line with the function's own file has the full inclusive cost)
        best = {}
        for line in out.splitlines():
            m = re.match(r"\s*([\d,]+)\s+\(.*?\)\s+(.*)$", line) or re.match(r"\s*([\d,]+)\s+(.*)$", line)
            fn = m and rx.search(m.group(2))
            if fn:
                best[fn.group(0)] = max(best.get(fn.group(0), 0), int(m.group(1).replace(",", "")))
        info[group] = sum(best.values())
    return info


# counters from call counts (callee regex, caller regex or None): exact, and tangram needs no counter code
CALL_COUNTS = {
    "tile_builds": (r"Tangram::TileBuilder::build\(", None),
    "tile_requests": (r"loadTileData\(", r"Tangram::TileManager::loadTiles\("),
    # the collision test callback passed to isect2d in handleOcclusions (a std::function, so not inlined)
    "collision_candidates": (r"LabelManager::handleOcclusions.*_M_invoke\(", None),
    # exact and proxy raster lookups for tiles without their exact raster (cccd4277)
    "proxy_raster_lookups": (r"Tangram::RasterSource::getTexture\(", r"Tangram::TileManager::updateTileSet\("),
}


def call_counts(path):
    """Sum the calls= lines of a callgrind dump for the callees (and callers) in CALL_COUNTS."""
    names = {}
    rx = {k: (re.compile(ce), re.compile(cr) if cr else None) for k, (ce, cr) in CALL_COUNTS.items()}
    out = {k: 0 for k in CALL_COUNTS}
    fn = cfn = ""

    def name(spec):  # name compression: "(id) name" the first time, "(id)" after
        m = re.match(r"\((\d+)\)(?: (.*))?", spec)
        if not m:
            return spec
        if m.group(2):
            names[m.group(1)] = m.group(2)
        return names.get(m.group(1), "")

    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("fn="):
                fn = name(line[3:].strip())
            elif line.startswith("cfn="):
                cfn = name(line[4:].strip())
            elif line.startswith("calls="):
                n = int(line[6:].split()[0])
                for k, (ce, cr) in rx.items():
                    if ce.search(cfn) and (cr is None or cr.search(fn)):
                        out[k] += n
    return out


# inclusive cost groups; main thread frame work is in bench_main_* (replay.cpp)
COST_GROUPS = {
    "main_ir": r"bench_main_(frame|update|render|load_scene)\b",
    "tile_worker_ir": r"Tangram::TileWorker::run\(",
}


def cmd_callgrind(args):
    os.makedirs(args.out, exist_ok=True)
    for old in glob.glob(os.path.join(args.out, "callgrind.out*")):
        os.remove(old)
    wrapper = ["valgrind", "--tool=callgrind", "--fair-sched=yes",
               "--callgrind-out-file=" + os.path.join(args.out, "callgrind.out")]
    extra = (["--phases", args.phases] if args.phases else []) + \
        (["--stop-after", args.stop_after] if args.stop_after else [])
    rc, res = run_replay(args.fixture, extra, os.path.join(args.out, "replay.json"), wrapper=wrapper)
    phases = {}
    for path in sorted(glob.glob(os.path.join(args.out, "callgrind.out.*"))):
        info = parse_callgrind(path)
        m = re.match(r"Client Request: (.*)", info["trigger"])
        if not m:
            continue  # final dump at program exit
        phases[m.group(1)] = {"total_ir": info["ir"], "main_ir": info["main_ir"],
                              "tile_worker_ir": info["tile_worker_ir"], **call_counts(path),
                              "file": info["file"]}
    summary = {"run": res["run"], "fixture": fixture_info(args.fixture), "phases": phases}
    with open(os.path.join(args.out, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2)
    print(f"{'phase':16s} {'main Ir':>15s} {'tile worker Ir':>15s} {'total Ir':>15s} {'builds':>6s} "
          f"{'collisions':>10s} {'proxy lookups':>13s}")
    for name, p in phases.items():
        print(f"{name:16s} {p['main_ir']:>15,d} {p['tile_worker_ir']:>15,d} {p['total_ir']:>15,d} "
              f"{p['tile_builds']:>6d} {p['collision_candidates']:>10d} {p['proxy_raster_lookups']:>13d}")
    misses = offline_check(res)
    if misses:
        print(f"WARNING: tiles missing from fixture: {misses}", file=sys.stderr)
    return rc


def fixture_info(fixture):
    try:
        with open(os.path.join(fixture, "fixture.json")) as f:
            return json.load(f)
    except OSError:
        return {}


def cmd_memory(args):
    os.makedirs(args.out, exist_ok=True)
    env = dict(os.environ, MALLOC_ARENA_MAX="1")
    extra = (["--phases", args.phases] if args.phases else []) + \
        (["--stop-after", args.stop_after] if args.stop_after else [])
    runs = []
    for i in range(args.runs):
        rc, res = run_replay(args.fixture, extra, os.path.join(args.out, f"run{i}.json"), env=env)
        if rc != 0:
            die(f"run {i} failed")
        runs.append(res)
    phases = []
    for idx, p0 in enumerate(runs[0]["phases"]):
        merged = {}
        for k, v in p0.items():
            vals = [r["phases"][idx][k] for r in runs]
            if k in NOISY_FIELDS:
                merged[k] = statistics.median(vals)
            else:
                merged[k] = v
                if any(x != v for x in vals):
                    merged.setdefault("nondeterministic", []).append(k)
        phases.append(merged)
    summary = {"run": runs[0]["run"], "runs": args.runs, "fixture": fixture_info(args.fixture),
               "phases": phases}
    with open(os.path.join(args.out, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2)
    mb = 1 << 20
    print(f"{'phase':16s} {'heap MB':>8s} {'RSS pk MB':>9s} {'GPU tex MB':>10s} {'GPU buf MB':>10s} "
          f"{'labels':>7s} {'frames':>6s}")
    for p in phases:
        print(f"{p['phase']:16s} {p['heap_live']/mb:8.1f} {p['rss_peak']/mb:9.1f} {p['gpu_tex_bytes']/mb:10.1f} "
              f"{p['gpu_buf_bytes']/mb:10.1f} {p['labels_peak']:7.0f} {p['frames']:6.0f}"
              + (f"   nondeterministic: {', '.join(p['nondeterministic'])}" if "nondeterministic" in p else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ["fetch", "id", "prune", "callgrind", "memory"]:
        sp = sub.add_parser(name)
        sp.add_argument("--fixture", required=True)
        sp.add_argument("--workers", default="1",
                        help="tile worker threads (default 1: deterministic; tile workers share a glyph atlas)")
        if name == "fetch":
            sp.add_argument("--resume", action="store_true", help="continue an incomplete fetch")
        if name in ("callgrind", "memory"):
            sp.add_argument("--out", required=True)
            sp.add_argument("--phases", help="comma separated phases to run and measure (default: all; others are skipped except scene loads)")
            sp.add_argument("--stop-after", help="stop after this phase")
        if name == "memory":
            sp.add_argument("--runs", type=int, default=3)
    args = ap.parse_args()
    global WORKERS
    WORKERS = args.workers
    rc = {"fetch": cmd_fetch, "id": cmd_id, "prune": cmd_prune, "callgrind": cmd_callgrind,
          "memory": cmd_memory}[args.cmd](args)
    sys.exit(rc if isinstance(rc, int) else 0)


if __name__ == "__main__":
    main()

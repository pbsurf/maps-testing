// Headless benchmark replay: loads the app's real scene against a fixed tile fixture, plays a scripted
//  camera path over Paris in phases, and reports per-phase memory and GPU stats as JSON.
// - links tangram core with the null GL driver in tests/src/gl_mock.cpp (no GPU or X server)
// - every frame first waits until no tile task is being fetched or built, so the set of tiles each frame
//   sees, and so the work done on every thread, is independent of thread timing
// - under callgrind, each phase is zeroed and dumped separately (CALLGRIND_DUMP_STATS_AT("<phase>"))
// - each phase's measured run is bounded by CLOCK_MONOTONIC t_start/t_end (for perf samples), and with
//   --counters reports per-thread hardware counters for it (counters.cpp; bench.py perf)
// See testing/bench/README.md for usage.

#include "map.h"
#include "log.h"
#include "scene/scene.h"
#include "data/rasterSource.h"
#include "labels/labelManager.h"
#include "tile/tile.h"
#include "tile/tileCache.h"
#include "tile/tileManager.h"
#include "util/asyncWorker.h"
#include "util/elevationManager.h"

#include "benchPlatform.h"
#include "counters.h"
#include "gl_mock.h"

#include <malloc.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__has_include)
#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#define HAVE_CALLGRIND 1
#endif
#endif
#ifndef HAVE_CALLGRIND
#define CALLGRIND_ZERO_STATS
#define CALLGRIND_DUMP_STATS_AT(s)
#define RUNNING_ON_VALGRIND 0
#endif

// functions normally provided by the platform layer
void TANGRAM_WakeEventLoop() {}

namespace Tangram {


void logStr(const std::string& msg) { fputs(msg.c_str(), stderr); }
// only TileWorker::run calls this, when a tile worker thread starts: name the thread (for perf record) and
//  open its counters (--counters)
void setCurrentThreadPriority(int priority) {
    pthread_setname_np(pthread_self(), "tile_worker");
    BenchCounters::addCurrentThread("tile_worker");
}
void initGLExtensions() {}

}

using namespace Tangram;

// Main thread work, in named functions so its cost can be separated from other threads in callgrind
//  output (bench.py reads their inclusive cost); noinline keeps them visible in optimized builds
extern "C" __attribute__((noinline)) uint32_t bench_main_frame(Map* map, float dt) {
    MapState state = map->update(dt);
    map->render();
    return state.flags;
}

// frame split in two, so the replay can wait for tile workers between update and render (see frame())
extern "C" __attribute__((noinline)) uint32_t bench_main_update(Map* map, float dt) {
    return map->update(dt).flags;
}

extern "C" __attribute__((noinline)) void bench_main_render(Map* map) {
    map->render();
}

extern "C" __attribute__((noinline)) void bench_main_load_scene(Map* map, SceneOptions* options) {
    map->loadScene(std::move(*options), false);
}

namespace {

constexpr float FRAME_DT = 1/60.0f;
// time step while waiting for a view to complete: labels fade out over 1s (scene hide transitions), which
//  would take 60 nearly identical frames at FRAME_DT
constexpr float SETTLE_DT = 0.25f;

struct Args {
    std::string assets = "assets";
    std::string fixture;
    std::string out;
    std::string source;  // default: sources.last_source from config.default.yaml
    std::vector<std::string> phases;  // default: all
    std::string stopAfter;  // stop after this phase
    bool fetch = false;
    int workers = 2;
    int width = 1600, height = 1000;
    float pixelScale = 1.0f;
    double settleTimeout = 60;  // seconds to wait for in-flight tile tasks before giving up
};

double nowSec(clockid_t clk) {
    timespec ts;
    clock_gettime(clk, &ts);
    return ts.tv_sec + ts.tv_nsec*1E-9;
}

std::string fmtTime(double t) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.9f", t);
    return buf;
}

// /proc/self/status value in bytes (fields are in kB), e.g. "VmHWM", "VmRSS"
size_t procStatus(const char* key) {
    std::ifstream f("/proc/self/status");
    std::string line;
    size_t keylen = strlen(key);
    while (std::getline(f, line)) {
        if (line.compare(0, keylen, key) == 0 && line[keylen] == ':') {
            return size_t(std::stoull(line.substr(keylen + 1)))*1024;
        }
    }
    return 0;
}

// reset VmHWM (peak RSS) to current RSS (Linux >= 4.0)
void resetPeakRss() {
    FILE* f = fopen("/proc/self/clear_refs", "w");
    if (f) { fputs("5", f); fclose(f); }
}

std::string jsonStr(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { r += '\\'; r += c; }
        else if (c == '\n') { r += "\\n"; }
        else if (uint8_t(c) < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); r += buf; }
        else { r += c; }
    }
    return r + "\"";
}

// ordered list of key/value pairs written as a JSON object
struct JsonObj {
    std::vector<std::pair<std::string, std::string>> kv;
    JsonObj& add(const std::string& k, const std::string& v) { kv.emplace_back(k, jsonStr(v)); return *this; }
    JsonObj& add(const std::string& k, const char* v) { return add(k, std::string(v)); }
    JsonObj& add(const std::string& k, double v) {
        char buf[64];
        if (v == std::floor(v) && std::fabs(v) < 1E15) { snprintf(buf, sizeof(buf), "%.0f", v); }
        else { snprintf(buf, sizeof(buf), "%.6g", v); }
        kv.emplace_back(k, buf);
        return *this;
    }
    JsonObj& add(const std::string& k, bool v) { kv.emplace_back(k, v ? "true" : "false"); return *this; }
    JsonObj& addRaw(const std::string& k, const std::string& v) { kv.emplace_back(k, v); return *this; }
    std::string str(const std::string& indent = "") const {
        std::string s = "{";
        for (size_t i = 0; i < kv.size(); ++i) {
            s += (i ? ",\n" : "\n") + indent + "  " + jsonStr(kv[i].first) + ": " + kv[i].second;
        }
        return s + "\n" + indent + "}";
    }
};

std::string yamlToStr(const YAML::Node& node) {
    if (node.IsScalar()) { return node.as<std::string>(); }
    YAML::Writer emitter;
    emitter.flowLevel = 10;
    emitter.indent = 0;
    return emitter.convert(node);
}

// Subset of the app's SourceBuilder (app/src/mapsources.cpp): vector scene layers and their updates.
//  Raster url sources aren't supported since the default source doesn't use them.
struct SceneBuilder {
    const YAML::Node& sources;
    std::vector<std::string> imports;
    std::vector<SceneUpdate> updates;
    std::vector<std::string> keys;

    explicit SceneBuilder(const YAML::Node& s) : sources(s) {}

    bool addLayer(const std::string& key) {
        for (auto& k : keys) { if (k == key) return true; }
        const YAML::Node& src = sources[key];
        if (!src) { LOGE("Invalid map source %s", key.c_str()); return false; }
        keys.push_back(key);
        for (const auto& layer : src["layers"]) {
            if (!addLayer(layer.IsMap() ? layer["source"].Scalar() : layer.Scalar())) { return false; }
        }
        if (src["url"]) {
            LOGE("Raster source %s not supported by benchmark replay", key.c_str());
            return false;
        }
        if (src["scene"]) { imports.push_back(src["scene"].Scalar()); }
        for (const auto& upd : src["updates"].pairs()) {
            updates.emplace_back("+" + upd.first.Scalar(), yamlToStr(upd.second));
        }
        for (const auto& upd : src["updates_vector"].pairs()) {
            updates.emplace_back("+" + upd.first.Scalar(), yamlToStr(upd.second));
        }
        return true;
    }
};


class Replay {
public:
    Args args;
    YAML::Node config;
    YAML::Node mapSources;
    BenchPlatform* platform = nullptr;
    std::unique_ptr<Map> map;
    std::string sceneYaml;
    std::vector<SceneUpdate> sceneUpdates;
    std::string baseUrl;

    // per-phase state
    uint64_t frames = 0;
    size_t labelsPeak = 0;
    bool incomplete = false;
    bool failed = false;
    bool stopped = false;
    std::vector<std::string> phaseResults;

    explicit Replay(const Args& _args) : args(_args) {
        // tile building is ~50x slower under valgrind
        if (RUNNING_ON_VALGRIND) { args.settleTimeout *= 50; }
    }

    bool init();
    bool buildScene(const std::string& source, bool terrain3D);
    bool loadScene(bool terrain3D, double lng, double lat, float zoom);
    void settle();
    void drain(const std::function<void(std::function<void()>)>& enqueue, const char* name);
    void setView(double lng, double lat, float zoom);
    bool tasksInFlight();
    MapState frame(float dt = FRAME_DT);
    void waitComplete(int maxFrames = 1200);
    void runFrames(int n, const std::function<void(float)>& step);
    void runPhase(const std::string& name, const std::function<void()>& setup,
                  const std::function<void()>& run);
    bool wantPhase(const std::string& name) const;
    void sampleLabels();
    void run();
    std::string headerJson() const;
};

bool Replay::init() {
    std::string cfgPath = args.assets + "/config.default.yaml";
    config = YAML::LoadFile(cfgPath);
    if (!config) { LOGE("Unable to load %s", cfgPath.c_str()); return false; }
    std::string srcPath = args.assets + "/mapsources.default.yaml";
    mapSources = YAML::LoadFile(srcPath);
    if (!mapSources) { LOGE("Unable to load %s", srcPath.c_str()); return false; }
    if (args.source.empty()) { args.source = config["sources"]["last_source"].as<std::string>(""); }

    char cwd[PATH_MAX];
    std::string assetsAbs = args.assets;
    if (assetsAbs[0] != '/' && getcwd(cwd, sizeof(cwd))) { assetsAbs = std::string(cwd) + "/" + assetsAbs; }
    baseUrl = "file://" + assetsAbs + "/";

    std::string requestLog = args.fetch ? args.fixture + "/requests.tsv" : "";
    auto plat = std::make_unique<BenchPlatform>(args.fetch, requestLog);
    plat->defaultUserAgent = "Tangram/1.0 (benchmark replay)";
    platform = plat.get();
    // like the app, render terrain depth on a separate "offscreen GL" thread (no context needed with gl_mock)
    ElevationManager::offscreenWorker = std::make_unique<AsyncWorker>("Bench offscreen GL worker");
    map = std::make_unique<Map>(std::move(plat));
    map->setupGL();
    map->resize(args.width, args.height);
    map->setPixelScale(args.pixelScale);
    return true;
}

// mirrors MapsSources::rebuildSource() and SourceBuilder::getSceneYaml()
bool Replay::buildScene(const std::string& source, bool terrain3D) {
    const YAML::Node& src = mapSources[source];
    if (!src) { LOGE("Unknown map source %s", source.c_str()); return false; }
    SceneBuilder builder(mapSources);
    std::vector<SceneUpdate> srcUpdates;
    if (src["layers"] && !src["scene"]) {
        for (const auto& layer : src["layers"]) {
            if (!builder.addLayer(layer.IsMap() ? layer["source"].Scalar() : layer.Scalar())) { return false; }
        }
        for (const auto& upd : src["updates"].pairs()) {
            srcUpdates.emplace_back("+" + upd.first.Scalar(), yamlToStr(upd.second));
        }
    } else if (!builder.addLayer(source)) {
        return false;
    }
    if (terrain3D) {
        for (const auto& upd : config["terrain_3d"]["updates"].pairs()) {
            builder.updates.emplace_back("+" + upd.first.Scalar(), yamlToStr(upd.second));
        }
    }
    builder.updates.insert(builder.updates.end(), srcUpdates.begin(), srcUpdates.end());

    auto addImport = [&](const std::string& url) {
        sceneYaml += "  - " + (url.find("://") == std::string::npos ? baseUrl : "") + url + "\n";
    };
    sceneYaml = "import:\n";
    for (const auto& imp : config["sources"]["common_imports"]) { addImport(imp.Scalar()); }
    if (terrain3D) { addImport(config["terrain_3d"]["import"].as<std::string>()); }
    for (auto& url : builder.imports) { addImport(url); }
    sceneUpdates = std::move(builder.updates);
    return true;
}

// mirrors MapsApp::loadSceneFile() and the scene completion handling in MapsApp::mapUpdate()
bool Replay::loadScene(bool terrain3D, double lng, double lat, float zoom) {
    if (!buildScene(args.source, terrain3D)) { return false; }
    for (auto& upd : sceneUpdates) {
        if (upd.value.c_str()[0] == '#') { upd.value = '"' + upd.value + '"'; }
    }
    SceneOptions options(sceneYaml, Url(baseUrl + "__GUI_SOURCES__"), false, sceneUpdates);
    options.updates.push_back(SceneUpdate{"scene.elevation_source",
        config["sources"]["elevation"][0].as<std::string>("")});
    if (terrain3D) { options.updates.push_back(SceneUpdate{"scene.terrain_3d", "true"}); }
    options.updates.push_back(SceneUpdate{"global.metric_units", "true"});
    options.updates.push_back(SceneUpdate{"global.shuffle_seed", "0"});
    options.updates.push_back(SceneUpdate{"global.selected_osm_id", "~"});
    // the vector tile source has no disk cache in the app; the fixture needs one
    options.updates.push_back(SceneUpdate{"+sources.osm.cache", "osm"});
    options.diskTileCacheSize = 256*1024*1024;  // just > 0 to enable cache
    options.diskCacheDir = args.fixture + "/";
    // fixture tiles never expire (values > 2^30 are treated as absolute timestamps); sources with their own
    //  max_age (elevation: 2 years) still expire, so refresh the fixture before then
    options.diskTileCacheMaxAge = (int64_t(1) << 30) - 1;
    options.preserveMarkers = true;
    options.metricUnits = true;
    for (const auto& font : config["fallback_fonts"]) {
        options.fallbackFonts.push_back(FontSourceHandle(Url(args.assets + "/" + font.Scalar())));
    }
    options.numTileWorkers = args.workers;

    bench_main_load_scene(map.get(), &options);
    setView(lng, lat, zoom);
    map->getScene()->tileManager()->setCacheSize(config["tile_cache_limit"].as<size_t>(512*1024*1024));

    // run frames until the scene is complete (resources are read synchronously, so this is quick)
    double t0 = nowSec(CLOCK_MONOTONIC);
    while (!map->getScene()->isReady()) {
        settle();
        bench_main_frame(map.get(), FRAME_DT);
        ++frames;
        if (map->getScene()->isReady()) { break; }
        if (nowSec(CLOCK_MONOTONIC) - t0 > args.settleTimeout) {
            auto* err = map->getScene()->errors();
            LOGE("Scene failed to load%s%s", err ? ": " : "", err ? err->update.path.c_str() : "");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (terrain3D) {
        auto* elevMgr = map->getScene()->elevationManager();
        if (elevMgr) {
            elevMgr->m_terrainScale = map->getScene()->config()["global"]["terrain_3d_scale"].as<float>(1.0f);
        }
    }
    return true;
}

bool Replay::tasksInFlight() {
    return map->getScene()->tileManager()->tasksInFlight();
}

// set zoom before position: the view constraint clamps latitude for the current zoom (at low zoom, the
//  whole world must fill the viewport)
void Replay::setView(double lng, double lat, float zoom) {
    map->setZoom(zoom);
    map->setPosition(lng, lat);
}

// wait (without running frames) until no dispatched tile task is still loading or building
void Replay::settle() {
    auto* tileMgr = map->getScene()->tileManager();
    double t0 = nowSec(CLOCK_MONOTONIC);
    while (tileMgr->tasksInFlight()) {
        if (nowSec(CLOCK_MONOTONIC) - t0 > args.settleTimeout) {
            LOGE("Timed out waiting for tile tasks");
            failed = true;
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    // ElevationManager skips the terrain depth pass for a frame if the previous one is still reading pixels
    //  on the offscreen worker, so wait for it
    if (auto& worker = ElevationManager::offscreenWorker) {
        drain([&](std::function<void()> f){ worker->enqueue(std::move(f)); }, "offscreen worker");
    }
    // Map::loadScene deletes the old scene on the async worker; wait so that its GPU resources are always
    //  released at the same point (otherwise e.g. gpu_tex_peak for terrain_load varies)
    drain([&](std::function<void()> f){ map->runAsyncTask(std::move(f)); }, "async worker");
}

// wait until a single thread task queue has run everything enqueued before this call
void Replay::drain(const std::function<void(std::function<void()>)>& enqueue, const char* name) {
    auto m = std::make_shared<std::mutex>();
    auto cv = std::make_shared<std::condition_variable>();
    auto done = std::make_shared<bool>(false);
    enqueue([=]() {
        std::lock_guard<std::mutex> lock(*m);
        *done = true;
        cv->notify_all();
    });
    std::unique_lock<std::mutex> lock(*m);
    if (!cv->wait_for(lock, std::chrono::duration<double>(args.settleTimeout), [&]{ return *done; })) {
        LOGE("Timed out waiting for %s", name);
        failed = true;
    }
}

void Replay::sampleLabels() {
    labelsPeak = std::max(labelsPeak, map->getScene()->labelManager()->numLabels());
}

MapState Replay::frame(float dt) {
    settle();
    MapState state;
    state.flags = bench_main_update(map.get(), dt);
    // update() dispatches tile tasks; wait for them before render, otherwise how many new glyphs the
    //  glyph atlas upload in render picks up depends on worker timing. Results are only taken by the
    //  next update(), so this doesn't change what the frame shows.
    settle();
    bench_main_render(map.get());
    ++frames;
    sampleLabels();
    return state;
}

// pause: run frames until all tiles are loaded and labels have stopped animating
// - label fade-in only shows up in MapState the frame after tiles complete, so require two consecutive
//   complete frames
void Replay::waitComplete(int maxFrames) {
    static bool verbose = getenv("BENCH_VERBOSE") != nullptr;
    int ncomplete = 0;
    for (int i = 0; i < maxFrames && !failed; ++i) {
        MapState state = frame(SETTLE_DT);
        if (verbose) {
            LOG("waitComplete frame %d: flags %x, labels %d, tiles loading %d, visible %d",
                i, state.flags, int(map->getScene()->labelManager()->numLabels()),
                map->getScene()->tileManager()->numLoadingTiles(),
                int(map->getScene()->tileManager()->getVisibleTiles().size()));
        }
        ncomplete = state.viewComplete() && !tasksInFlight() ? ncomplete + 1 : 0;
        if (ncomplete >= 2) { return; }
    }
    LOGW("View still not complete after %d frames", maxFrames);
    incomplete = true;
}

// run n frames, calling step(t) with t from 1/n to 1 before each one
void Replay::runFrames(int n, const std::function<void(float)>& step) {
    for (int i = 1; i <= n && !failed; ++i) {
        step(float(i)/n);
        frame();
    }
}

bool Replay::wantPhase(const std::string& name) const {
    if (args.phases.empty()) { return true; }
    return std::find(args.phases.begin(), args.phases.end(), name) != args.phases.end();
}

void Replay::runPhase(const std::string& name, const std::function<void()>& setup,
                      const std::function<void()>& run) {
    if (failed || stopped) { return; }
    if (name == args.stopAfter) { stopped = true; }
    bool measure = wantPhase(name);
    bool isSceneLoad = name == "startup" || name == "terrain_load";
    if (!measure && !args.fetch && !isSceneLoad) { return; }
    platform->setPhase(name);
    // setup (camera jumps, settling) is not measured
    if (setup) { setup(); }
    if (!measure && !args.fetch) {
        // phases not requested with --phases are skipped, except the scene loads later phases need; each
        //  phase's setup puts the camera where its run expects it, but tile caches start colder, so only
        //  compare runs with the same --phases
        if (isSceneLoad) { run(); }
        return;
    }

    LOG("=== Phase %s", name.c_str());
    frames = 0;
    labelsPeak = 0;
    incomplete = false;
    uint64_t net0 = platform->networkRequests, netfail0 = platform->networkFailures;
    GLMockStats gl0 = glMockStats();
    glMockResetPeaks();
    resetPeakRss();
    double wall0 = nowSec(CLOCK_MONOTONIC);
    double cpu0 = nowSec(CLOCK_THREAD_CPUTIME_ID);
    double pcpu0 = nowSec(CLOCK_PROCESS_CPUTIME_ID);

    BenchCounters::begin();
    CALLGRIND_ZERO_STATS;
    run();
    CALLGRIND_DUMP_STATS_AT(name.c_str());
    std::string counters = BenchCounters::endJson();

    double cpu = nowSec(CLOCK_THREAD_CPUTIME_ID) - cpu0;
    double pcpu = nowSec(CLOCK_PROCESS_CPUTIME_ID) - pcpu0;
    double wall = nowSec(CLOCK_MONOTONIC) - wall0;
    size_t rssPeak = procStatus("VmHWM");
    size_t rss = procStatus("VmRSS");
    struct mallinfo2 mi = mallinfo2();
    const GLMockStats& gl = glMockStats();

    auto* scene = map->getScene();
    auto* tileMgr = scene->tileManager();
    size_t visibleTileBytes = 0;
    for (auto& tile : tileMgr->getVisibleTiles()) { visibleTileBytes += tile->getMemoryUsage(); }
    size_t rasterBytes = 0, rasterTextures = 0;
    for (auto& src : scene->tileSources()) {
        if (!src->isRaster()) { continue; }
        size_t n = 0;
        rasterBytes += static_cast<RasterSource*>(src.get())->cacheMemoryUsage(&n);
        rasterTextures += n;
    }
    double nf = std::max(uint64_t(1), frames);

    JsonObj r;
    r.add("phase", name)
     .add("frames", double(frames))
     .add("complete", !incomplete && !failed)
     .add("wall_s", wall)
     // CLOCK_MONOTONIC bounds of the measured run, to match perf samples (perf record -k monotonic)
     .addRaw("t_start", fmtTime(wall0))
     .addRaw("t_end", fmtTime(wall0 + wall))
     .add("main_cpu_s", cpu)
     .add("process_cpu_s", pcpu)
     .add("heap_live", double(mi.uordblks))
     .add("heap_arena", double(mi.arena + mi.hblkhd))
     .add("heap_mmap", double(mi.hblkhd))
     .add("rss", double(rss))
     .add("rss_peak", double(rssPeak))
     .add("gpu_tex_bytes", double(gl.textureBytes))
     .add("gpu_buf_bytes", double(gl.bufferBytes))
     .add("gpu_rb_bytes", double(gl.renderbufferBytes))
     .add("gpu_tex_peak", double(gl.textureBytesPeak))
     .add("gpu_buf_peak", double(gl.bufferBytesPeak))
     .add("gpu_textures", double(gl.textures))
     .add("gpu_buffers", double(gl.buffers))
     .add("tex_uploads", double(gl.textureUploads - gl0.textureUploads))
     .add("tex_upload_bytes", double(gl.textureUploadBytes - gl0.textureUploadBytes))
     .add("buf_uploads", double(gl.bufferUploads - gl0.bufferUploads))
     .add("buf_upload_bytes", double(gl.bufferUploadBytes - gl0.bufferUploadBytes))
     .add("draw_calls_per_frame", (gl.drawCalls - gl0.drawCalls)/nf)
     .add("visible_tiles", double(tileMgr->getVisibleTiles().size()))
     .add("visible_tile_bytes", double(visibleTileBytes))
     .add("tile_cache_bytes", double(tileMgr->getTileCache()->getMemoryUsage()))
     .add("tile_cache_entries", double(tileMgr->getTileCache()->getNumEntries()))
     .add("raster_cache_bytes", double(rasterBytes))
     .add("raster_textures", double(rasterTextures))
     .add("labels_peak", double(labelsPeak))
     .add("network_requests", double(platform->networkRequests - net0));
    if (BenchCounters::enabled()) { r.addRaw("counters", counters); }
    if (args.fetch) { r.add("network_failures", double(platform->networkFailures - netfail0)); }
    phaseResults.push_back(r.str("    "));
    LOG("=== Phase %s: %d frames, main thread CPU %.2fs", name.c_str(), int(frames), cpu);
}

// BENCH_* environment variables, e.g. switches added by patches from bench/patches, so results record them
static std::string benchEnv() {
    std::vector<std::string> vars;
    for (char** e = environ; *e; ++e) {
        if (strncmp(*e, "BENCH_", 6) == 0) { vars.push_back(*e); }
    }
    std::sort(vars.begin(), vars.end());
    std::string out;
    for (auto& v : vars) { out += (out.empty() ? "" : " ") + v; }
    return out;
}

std::string Replay::headerJson() const {
    JsonObj h;
    h.add("mode", args.fetch ? "fetch" : "offline")
     .add("fixture", args.fixture)
     .add("source", args.source)
     .add("workers", double(args.workers))
     .add("width", double(args.width))
     .add("height", double(args.height))
     .add("pixel_scale", double(args.pixelScale))
     .add("valgrind", bool(RUNNING_ON_VALGRIND))
     .add("counters", BenchCounters::enabled())
     .add("bench_env", benchEnv())
     .add("phases", [&]{ std::string p; for (auto& s : args.phases) { p += (p.empty() ? "" : ",") + s; } return p; }())
     .add("malloc_arena_max", getenv("MALLOC_ARENA_MAX") ? getenv("MALLOC_ARENA_MAX") : "");
    return h.str("  ");
}

// The Paris script. Positions: (lng, lat).
// The tile cache only holds about one screen of tiles, so the script never returns to an area at a tile
//  zoom level it has already built (that would measure the same tile builds again). Each frame waits for
//  its tiles, so every frame's view is fully built: frames are only spent where they add new work.
void Replay::run() {
    const double PARIS_LNG = 2.352, PARIS_LAT = 48.857;

    auto lerp = [](double a, double b, float t) { return a + (b - a)*t; };
    auto panTo = [&](double lng0, double lat0, double lng1, double lat1, int n) {
        runFrames(n, [&](float t) { map->setPosition(lerp(lng0, lng1, t), lerp(lat0, lat1, t)); });
    };

    // 1. startup and scene load, centered on Paris at z14 (the z14 tiles cover about 13 x 8 km)
    runPhase("startup", nullptr, [&]() {
        if (!loadScene(false, PARIS_LNG, PARIS_LAT, 14)) { failed = true; return; }
        waitComplete();
    });

    // 2. pan west to east along the Seine through central Paris at z15.5 (dense labels: handleOcclusions),
    //  about one view width (3.5 km), so the tiles and labels in view turn over once. The start view is
    //  loaded in setup: only tiles brought in by panning are measured
    const double PAN_LAT = 48.8566, PAN_LNG0 = 2.3150, PAN_LNG1 = 2.3636;
    runPhase("pan", [&]() {
        setView(PAN_LNG0, PAN_LAT, 15.5f);  // Concorde
        waitComplete();
    }, [&]() {
        panTo(PAN_LNG0, PAN_LAT, PAN_LNG1, PAN_LAT, 30);  // to the Marais
        waitComplete();
    });

    // 3. idle: steady-state frames with nothing loading, where pan ended (2D, before terrain mode)
    runPhase("idle", [&]() {
        setView(PAN_LNG1, PAN_LAT, 15.5f);  // no new tiles after pan
        waitComplete();
    }, [&]() {
        runFrames(60, [](float) {});
    });

    // 4. zoom out from z17 to z10 north of Paris (Saint-Denis), clear of the tiles built by startup and
    //  pan. Three frames per tile zoom level L, at L+0.9, L+0.7 and L+0.5: frames within a level show a
    //  subset of the tiles at L+0.5, so this builds every level once at about half the tiles of a view at
    //  L+0.0, which a continuous zoom would pass through
    runPhase("zoom", nullptr, [&]() {
        setView(2.3600, 48.9500, 17.9f);
        for (int z = 17; z >= 10 && !failed; --z) {
            for (float f : {0.9f, 0.7f, 0.5f}) {
                map->setZoom(z + f);
                frame();
            }
        }
        waitComplete();
    });

    // 5. scene reload in 3D terrain mode
    runPhase("terrain_load", nullptr, [&]() {
        if (!loadScene(true, PARIS_LNG, PARIS_LAT, 15)) { failed = true; return; }
        waitComplete();
    });

    // 6. tilt in terrain mode (Paris is flat: measures terrain overhead). No rotation: it only changes the
    //  view matrix, but brings new tiles into view and makes tiles switch zoom levels (rebuilds)
    runPhase("terrain", nullptr, [&]() {
        runFrames(10, [&](float t) { map->setTilt(float(M_PI/3)*t); });
        waitComplete();
    });

    // 7. missing rasters: elevation tiles at z >= 14 for this area are deleted from the fixture (see
    //  bench.py prune; only tiles no other phase uses), so tiles use proxy rasters from the z12-13
    //  elevation tiles loaded in setup
    runPhase("missing_rasters", [&]() {
        map->setTilt(0);
        setView(2.6000, 48.8500, 12.5f);  // about 18 km east, outside the area terrain's tilted view reaches
        waitComplete();
    }, [&]() {
        map->setZoom(15);  // tiles find proxies (parent search)
        waitComplete();
        runFrames(10, [](float) {});  // steady state on proxies (per-frame exact raster lookups)
    });
}

void usage() {
    fprintf(stderr,
        "Usage: replay --fixture DIR [options]\n"
        "  --fixture DIR      tile fixture directory (mbtiles cache files)\n"
        "  --fetch            download missing tiles into the fixture (logs requests to DIR/requests.tsv)\n"
        "  --assets DIR       app assets directory (default: assets)\n"
        "  --out FILE         write JSON results to FILE (default: stdout)\n"
        "  --phases A,B,...   only run and measure these phases (and the scene loads they need)\n"
        "  --stop-after PHASE stop after this phase\n"
        "  --source NAME      map source (default: sources.last_source in config.default.yaml)\n"
        "  --workers N        tile worker threads (default: 2)\n"
        "  --size WxH         viewport size (default: 1600x1000)\n"
        "  --pixel-scale S    pixel scale (default: 1)\n"
        "  --settle-timeout S seconds to wait for tile tasks (default: 60)\n"
        "  --counters         per-phase hardware counters per thread group (perf_event_open)\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    Args args;
    bool counters = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { usage(); exit(2); }
            return argv[++i];
        };
        if (a == "--fixture") { args.fixture = next(); }
        else if (a == "--fetch") { args.fetch = true; }
        else if (a == "--counters") { counters = true; }
        else if (a == "--stop-after") { args.stopAfter = next(); }
        else if (a == "--assets") { args.assets = next(); }
        else if (a == "--out") { args.out = next(); }
        else if (a == "--source") { args.source = next(); }
        else if (a == "--workers") { args.workers = std::stoi(next()); }
        else if (a == "--pixel-scale") { args.pixelScale = std::stof(next()); }
        else if (a == "--settle-timeout") { args.settleTimeout = std::stod(next()); }
        else if (a == "--size") {
            std::string s = next();
            if (sscanf(s.c_str(), "%dx%d", &args.width, &args.height) != 2) { usage(); return 2; }
        }
        else if (a == "--phases") {
            std::stringstream ss(next());
            std::string p;
            while (std::getline(ss, p, ',')) { if (!p.empty()) { args.phases.push_back(p); } }
        }
        else { usage(); return 2; }
    }
    if (args.fixture.empty()) { usage(); return 2; }
    if (access(args.fixture.c_str(), W_OK) != 0) {
        fprintf(stderr, "Fixture directory %s does not exist or is not writable\n", args.fixture.c_str());
        return 2;
    }

    // before any other thread starts: the main thread's counters, then each tile worker opens its own
    if (counters && !BenchCounters::enable()) {
        fprintf(stderr, "--counters: hardware counters unavailable (kernel.perf_event_paranoid <= 2?)\n");
        return 1;
    }
    Replay replay(args);
    if (!replay.init()) { return 1; }
    replay.run();

    uint64_t netRequests = replay.platform->networkRequests;
    uint64_t netFailures = replay.platform->networkFailures;
    if (args.fetch) {
        // MBTilesDataSource writes tiles to the cache on its own thread and drops queued writes when
        //  destroyed, so give it time to finish
        LOG("Waiting for tile cache writes...");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    replay.map.reset();
    if (auto worker = std::move(ElevationManager::offscreenWorker)) {
        worker->enqueue([](){ ElevationManager::shutdown(); });
        worker->waitForCompletion();
    }

    std::string json = "{\n  \"run\": " + replay.headerJson() + ",\n  \"phases\": [\n";
    for (size_t i = 0; i < replay.phaseResults.size(); ++i) {
        json += (i ? ",\n    " : "    ") + replay.phaseResults[i];
    }
    json += "\n  ]\n}\n";
    if (args.out.empty()) {
        fputs(json.c_str(), stdout);
    } else {
        std::ofstream f(args.out);
        f << json;
    }

    if (replay.failed) { return 1; }
    if (args.fetch && netFailures > 0) {
        fprintf(stderr, "%llu of %llu tile requests failed; run --fetch again to retry\n",
                (unsigned long long)netFailures, (unsigned long long)netRequests);
        return 1;
    }
    return 0;
}

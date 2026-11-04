// Standalone test driver for app/src/offlinerouter.cpp (router core only, no app code)
// build (from explore root): make -f testing/routing.mk  (builds build/Routing/routetest)
// run: build/Routing/routetest <routing yaml> <tiles dir with {z}/{x}_{y}.mvt> <walk|bike|drive> lng lat lng lat [...]
// - routing yaml: file with application.routing, e.g. assets/scenes/routing.yaml
// - prints summary and directions to stderr, GeoJSON LineString to stdout; TWICE=1 routes again on the cached graph
// - Andorra test tiles: ~/maps/testdata/andorra-tiles
// - ELEV=<dir with {z}/{x}_{y}.lerc> uses elevation tiles (Andorra z12: ~/maps/testdata/andorra-elev); route points
//  are then printed with elevation, and ascent/descent is included in summary
// - CACHE=<dir> loads tiles through tangram TileSources like the app (loadTileSync(): mbtiles cache in <dir>, then
//  "network" via MockPlatform, which reads URLs from the file system); run twice to test cache hits
#define OFFLINE_ROUTER_STANDALONE
#include "../../app/src/offlinerouter.cpp"
#include "util/zlibHelper.h"
#include "data/mbtilesDataSource.h"
#include "data/networkDataSource.h"
#include "mockPlatform.h"
#include <fstream>
#include <sstream>
#include <chrono>
using namespace OfflineRouter;

using namespace Tangram;

static std::string dir;
static int nload = 0, nelev = 0;
static double loadMs = 0;  // time spent in tile loaders

static std::string readFile(const std::string& d, int x, int y, int z, const char* ext)
{
  std::ifstream f(d + "/" + std::to_string(z) + "/" + std::to_string(x) + "_" + std::to_string(y) + ext, std::ios::binary);
  if(!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static std::string loadFile(int x, int y, int z);
static std::string load(int x, int y, int z)
{
  auto t = std::chrono::steady_clock::now();
  std::string data = loadFile(x, y, z);
  loadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
  return data;
}

static std::string loadFile(int x, int y, int z)
{
  ++nload;
  std::string in = readFile(dir, x, y, z, ".mvt");
  if(in.empty()) return {};
  std::vector<char> out;
  if(Tangram::zlib_inflate(in.data(), in.size(), out) != 0) return in;
  return std::string(out.data(), out.size());
}

int main(int argc, char** argv)
{
  if(argc < 8) {
    fprintf(stderr, "usage: routetest <routing yaml> <tiles dir> <walk|bike|drive> lng lat lng lat [...]\n");
    return 1;
  }
  auto cfg = std::make_shared<Config>();
  std::string err;
  if(!parseConfig(YAML::LoadFile(argv[1])["application"]["routing"], *cfg, err)) {
    fprintf(stderr, "Config error: %s\n", err.c_str());
    return 1;
  }
  dir = argv[2];
  std::string m = argv[3];
  Mode mode = m == "walk" ? FOOT : m == "bike" ? BIKE : CAR;
  std::vector<std::pair<double,double>> pts;
  for(int i = 4; i + 1 < argc; i += 2)
    pts.emplace_back(atof(argv[i]), atof(argv[i+1]));
  Router r(cfg, load);
  const char* elevDir = getenv("ELEV");
  if(elevDir) {
    std::string edir = elevDir;
    r.setElevLoader([edir](int x, int y, int z){
      ++nelev;
      auto t = std::chrono::steady_clock::now();
      std::string data = readFile(edir, x, y, z, ".lerc");
      loadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
      return data;
    }, cfg->elevZoom);
  }
  // load through TileSources: cache, then file system "network"
  MockPlatform platform;
  YAML::Node globals;
  DataSourceContext dsctx(platform, globals);
  auto prana = std::make_shared<ScenePrana>(nullptr);
  std::shared_ptr<TileSource> src, elevSrc;
  auto makeSource = [&](const std::string& name, const std::string& urlTemplate) {
    auto net = std::make_unique<NetworkDataSource>(dsctx, urlTemplate, UrlOptions());
    auto mb = std::make_unique<MBTilesDataSource>(platform, name, std::string(getenv("CACHE")) + "/" + name + ".mbtiles",
        "", 365*86400);
    mb->next = std::move(net);
    return std::make_shared<TileSource>(name, std::move(mb));
  };
  auto notCanceled = [](){ return false; };
  if(getenv("CACHE")) {
    src = makeSource("osm", "file://" + dir + "/{z}/{x}_{y}.mvt");
    r.setLoader([&](int x, int y, int z){
      ++nload;
      auto t = std::chrono::steady_clock::now();
      std::string data = loadTileSync(*src, prana, TileID(x, y, z), notCanceled);
      loadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
      return data;
    });
    if(elevDir) {
      elevSrc = makeSource("elevation", "file://" + std::string(elevDir) + "/{z}/{x}_{y}.lerc");
      r.setElevLoader([&](int x, int y, int z){
        ++nelev;
        auto t = std::chrono::steady_clock::now();
        std::string data = loadTileSync(*elevSrc, prana, TileID(x, y, z), notCanceled);
        loadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        return data;
      }, cfg->elevZoom);
    }
  }
  auto t0 = std::chrono::steady_clock::now();
  auto res = r.route(mode, pts);
  if(getenv("TWICE")) { nload = nelev = 0; loadMs = 0; t0 = std::chrono::steady_clock::now(); res = r.route(mode, pts); }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  double up = 0, down = 0;
  for(size_t i = 1; i < res.pts.size(); ++i) {
    double dz = res.pts[i].elev - res.pts[i-1].elev;
    if(dz > 0) up += dz; else if(dz < 0) down -= dz;
  }
  fprintf(stderr, "%s: %zu pts, %.0f m, %.0f s, +%.0f/-%.0f m, tiles loaded %d, elev tiles %d, %.1f ms (%.1f ms loading)\n",
      res.error.c_str(), res.pts.size(), res.pts.empty() ? 0 : res.pts.back().dist,
      res.pts.empty() ? 0 : res.pts.back().time, up, down, nload, nelev, ms, loadMs);
  for(auto& s : res.steps)
    fprintf(stderr, "  [%d %.0fm] %s\n", s.idx, res.pts[s.idx].dist, s.instr.c_str());
  printf("{\"type\":\"LineString\",\"coordinates\":[");
  for(size_t i = 0; i < res.pts.size(); ++i)
    if(std::isnan(res.pts[i].elev)) printf("%s[%.6f,%.6f]", i ? "," : "", res.pts[i].lng, res.pts[i].lat);
    else printf("%s[%.6f,%.6f,%.1f]", i ? "," : "", res.pts[i].lng, res.pts[i].lat, res.pts[i].elev);
  printf("]}\n");
  return 0;
}

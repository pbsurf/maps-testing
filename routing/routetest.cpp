// Standalone test driver for app/src/offlinerouter.cpp (router core only, no app code)
// build (from explore root): make -f testing/routing.mk  (builds build/Routing/routetest)
// run: build/Routing/routetest <routing yaml> <tiles dir with {z}/{x}_{y}.mvt> <walk|bike|drive> lng lat lng lat [...]
// - routing yaml: file with application.routing, e.g. assets/scenes/routing.yaml
// - prints summary and directions to stderr, GeoJSON LineString to stdout; TWICE=1 routes again on the cached graph
// - Andorra test tiles: ~/maps/testdata/andorra-tiles
#define OFFLINE_ROUTER_STANDALONE
#include "../../app/src/offlinerouter.cpp"
#include "util/zlibHelper.h"
#include <fstream>
#include <sstream>
#include <chrono>
using namespace OfflineRouter;

namespace Tangram {
void logStr(const std::string& msg) { fprintf(stderr, "%s", msg.c_str()); }  // normally provided by platform
}

static std::string dir;
static int nload = 0;

static std::vector<std::string> load(int x, int y, int z)
{
  ++nload;
  std::ifstream f(dir + "/" + std::to_string(z) + "/" + std::to_string(x) + "_" + std::to_string(y) + ".mvt", std::ios::binary);
  if(!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  std::string in = ss.str();
  std::vector<char> out;
  if(Tangram::zlib_inflate(in.data(), in.size(), out) != 0) return {in};
  return {std::string(out.data(), out.size())};
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
  auto t0 = std::chrono::steady_clock::now();
  auto res = r.route(mode, pts);
  if(getenv("TWICE")) { nload = 0; t0 = std::chrono::steady_clock::now(); res = r.route(mode, pts); }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  fprintf(stderr, "%s: %zu pts, %.0f m, %.0f s, tiles loaded %d, %.1f ms\n", res.error.c_str(), res.pts.size(),
      res.pts.empty() ? 0 : res.pts.back().dist, res.pts.empty() ? 0 : res.pts.back().time, nload, ms);
  for(auto& s : res.steps)
    fprintf(stderr, "  [%d %.0fm] %s\n", s.idx, res.pts[s.idx].dist, s.instr.c_str());
  printf("{\"type\":\"LineString\",\"coordinates\":[");
  for(size_t i = 0; i < res.pts.size(); ++i)
    printf("%s[%.6f,%.6f]", i ? "," : "", res.pts[i].lng, res.pts[i].lat);
  printf("]}\n");
  return 0;
}

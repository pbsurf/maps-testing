// Standalone test driver for app/src/offlinerouter.cpp (router core only, no app code)
// build (from explore root):
//   g++ -std=c++14 -O2 -Itangram-es/core/deps testing/routing/routetest.cpp -lz -o build/routetest
// run: build/routetest <tiles dir with {z}/{x}_{y}.mvt> <walk|bike|drive> lng lat lng lat [lng lat ...] > route.geojson
// - prints summary and directions to stderr, GeoJSON LineString to stdout; TWICE=1 routes again on the cached graph
// - Andorra test tiles: ~/maps/testdata/andorra-tiles
#define OFFLINE_ROUTER_STANDALONE
#include "../../app/src/offlinerouter.cpp"
#include <zlib.h>
#include <fstream>
#include <sstream>
#include <chrono>
using namespace OfflineRouter;
static std::string dir;
static int nload = 0;
std::vector<std::string> load(int x, int y, int z) {
  ++nload;
  std::ifstream f(dir + "/" + std::to_string(z) + "/" + std::to_string(x) + "_" + std::to_string(y) + ".mvt", std::ios::binary);
  if(!f) return {};
  std::stringstream ss; ss << f.rdbuf(); std::string in = ss.str();
  std::string out(1<<22, '\0');
  z_stream zs{}; inflateInit2(&zs, 15+32);
  zs.next_in = (Bytef*)in.data(); zs.avail_in = in.size();
  zs.next_out = (Bytef*)out.data(); zs.avail_out = out.size();
  inflate(&zs, Z_FINISH); out.resize(zs.total_out); inflateEnd(&zs);
  return {out};
}
int main(int argc, char** argv) {
  dir = argv[1];
  Mode mode = std::string(argv[2]) == "walk" ? FOOT : std::string(argv[2]) == "bike" ? BIKE : CAR;
  std::vector<std::pair<double,double>> pts;
  for(int i = 3; i + 1 < argc; i += 2) pts.emplace_back(atof(argv[i]), atof(argv[i+1]));
  Router r(load);
  auto t0 = std::chrono::steady_clock::now();
  auto res = r.route(mode, pts);
  if(getenv("TWICE")) { nload = 0; res = r.route(mode, pts); }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  fprintf(stderr, "%s: %zu pts, %.0f m, %.0f s, tiles loaded %d, %.1f ms\n", res.error.c_str(), res.pts.size(),
      res.pts.empty() ? 0 : res.pts.back().dist, res.pts.empty() ? 0 : res.pts.back().time, nload, ms);
  for(auto& s : res.steps) fprintf(stderr, "  [%d %.0fm] %s\n", s.idx, res.pts[s.idx].dist, s.instr.c_str());
  printf("{\"type\":\"LineString\",\"coordinates\":[");
  for(size_t i = 0; i < res.pts.size(); ++i) printf("%s[%.6f,%.6f]", i ? "," : "", res.pts[i].lng, res.pts[i].lat);
  printf("]}\n");
}

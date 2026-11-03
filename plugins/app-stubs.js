// stubs for app functions provided to plugins by PluginManager::createFns() (app/src/plugins.cpp)
// - httpRequest responses come from httpResponses: {urlSubstring: filename or object (sent as JSON)};
//   requests are queued and run by runRequests() to mimic async behavior
// - PRINT_REQUESTS=1 prints each unhandled request as "REQUEST\t<url>\t<headers>" (used by fetch-data.sh)
var httpResponses = {}, httpQueue = [], searchResults = [], placeInfo = [], registeredFns = [];
function registerFunction(name, type, title) { registeredFns.push({name: name, type: type, title: title}); }
function notifyError(type, msg) { print("notifyError(" + type + "): " + msg); }
function httpRequest(url) {
  const cb = arguments[arguments.length - 1];
  httpQueue.push({url: url, cb: cb, headers: arguments.length > 2 ? arguments[1] : ""});
}
function runRequests() {
  while(httpQueue.length) {
    const req = httpQueue.shift();
    var resp = null;
    for(var key in httpResponses) { if(req.url.indexOf(key) >= 0) { resp = httpResponses[key]; break; } }
    if(resp) { req.cb(typeof resp == "string" ? readFile(resp) : JSON.stringify(resp), ""); }
    else {
      if(printRequests) { print("REQUEST\t" + req.url + "\t" + req.headers); }
      else { print("unhandled request: " + req.url.substr(0, 100)); }
      req.cb("", "no stub response");
    }
  }
}
function addSearchResult(id, lat, lng, score, flags, tags) {
  if(!(flags & 0x4000)) { searchResults.push({id: id, lat: lat, lng: lng, score: score, flags: flags, tags: tags}); }
}
function addPlaceInfo(icon, title, value) { placeInfo.push({icon: icon, title: title, value: value}); }
// remaining app functions: record calls so tests can inspect them if needed
var appCalls = [];
function addMapSource(key, yaml) { appCalls.push(["addMapSource", key, yaml]); }
function addBookmark(list, osm_id, name, props, notes, lnglat, date_created, date_modified) {
  appCalls.push(["addBookmark", list, osm_id, name, props, notes, lnglat, date_created, date_modified]);
}
function addRouteGPX(gpx) { appCalls.push(["addRouteGPX", gpx]); }
function addRoutePolyline(mode, polyline, precision) { appCalls.push(["addRoutePolyline", mode, polyline, precision]); }
function addRouteStep(instr, idx) { appCalls.push(["addRouteStep", instr, idx]); }
// scene/config values keyed by YAML path (e.g. "config.foo.bar"); app returns "" for missing values
var sceneValues = {};
function readSceneValue(path) { return path in sceneValues ? sceneValues[path] : ""; }
function writeSceneValue(path, value) { sceneValues[path] = value; }
function updateSceneLight(name, value) { appCalls.push(["updateSceneLight", name, value]); }
var cameraPosition = {longitude: 7.75, latitude: 48.58, zoom: 14, rotation: 0, tilt: 0};
function getCameraPosition() { return cameraPosition; }
function lookAt(lng, lat, zoom, rotation, tilt) { appCalls.push(["lookAt", lng, lat, zoom, rotation, tilt]); }
var secrets = {};
// test helpers
var verbose = !!getenv("VERBOSE");
var printRequests = !!getenv("PRINT_REQUESTS");
var failures = 0;
function check(cond, msg) { if(!cond) { failures++; print("FAIL: " + msg); } }

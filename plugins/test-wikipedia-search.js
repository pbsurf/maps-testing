// tests for wikipedia-search.js; run with `make` (VERBOSE=1 to list results)
// - fixed cases: wikidata records written inline, including real descriptions from Strasbourg (2026-11)
// - live data: data/wikidata-strasbourg.json from fetch-data.sh (not committed; skipped if missing). Wikidata
//   changes over time, so only checks that hold for any data are applied to it

const BOUNDS = [7.73, 48.57, 7.77, 48.59];  // central Strasbourg; fetch-data.sh downloads the query for this

function wdRecord(label, desc) {
  return {item: {value: "x"}, itemLabel: {value: label}, itemDescription: {value: desc}, wikiTitle: {value: "T"},
          url: {value: "u"}, lnglat: {value: "Point(7.75 48.58)"}, dist: {value: "0.1"}};
}

// [label, wikidata description, expected place type]
const cases = [
  ["Cathedral of Our Lady of Strasbourg", "cathedral located in Bas-Rhin, in France", "Cathedral"],  // "located in" cut
  ["Strasbourg-Ville station", "railway station in France", "Railway station"],  // "in France" cut
  ["Grande Île, Strasbourg", "island in Bas-Rhin, France", "Island"],  // accented capital in name
  ["Strasbourg", "capital and largest city of Grand Est, located in the historical region of Alsace",
   "Capital and largest city of Grand Est"],  // comma cut
  ["T0", "commune in Île-de-France, France", "Commune"],
  ["T1", "museum situated in Évry", "Museum"],
  ["T2", "organization founded in 1992", "Organization founded in 1992"],
  ["T3", "siege during the war in 1870", "Siege during the war in 1870"],
  ["T4", "castle based in the Ardennes", "Castle"],
  ["P0", "French university (1971-2008)", "French university"],
  ["P1", "university (founded 1621) in Strasbourg, France", "University"],
];

// checks that hold for any description
function checkType(t, label) {
  check(!/ (located|founded|situated|based)$/.test(t), "dangling participle: " + label + ": " + t);
  check(t.indexOf("(") < 0 && t.indexOf(",") < 0, "not truncated at ( or ,: " + label + ": " + t);
}

httpResponses = {"query.wikidata.org/sparql": {results: {bindings: cases.map(function(c) { return wdRecord(c[0], c[1]); })}}};
wikipediaSearch("", BOUNDS, 0);
runRequests();
check(searchResults.length == cases.length, "expected " + cases.length + " results, got " + searchResults.length);
for(var ii = 0; ii < searchResults.length; ii++) {
  const tags = searchResults[ii].tags;
  const t = getPlaceType(JSON.stringify(tags));
  const c = cases.filter(function(c) { return c[0] == tags.name; })[0];
  if(verbose) { print(tags.name + " | " + t); }
  check(c && t == c[2], tags.name + ": got '" + t + "', expected '" + (c && c[2]) + "'");
  checkType(t, tags.name);
}

// live data
var liveFile = "data/wikidata-strasbourg.json", haveLive = true;
try { readFile(liveFile); } catch(e) { haveLive = false; }
httpResponses = haveLive ? {"query.wikidata.org/sparql": liveFile} : {};
searchResults = [];
wikipediaSearch("", BOUNDS, 0);
runRequests();
if(haveLive) {
  check(searchResults.length > 0, "no results from " + liveFile);
  for(var ii = 0; ii < searchResults.length; ii++) {
    const tags = searchResults[ii].tags;
    const t = getPlaceType(JSON.stringify(tags));
    if(verbose) { print(tags.name + " | " + t); }
    checkType(t, tags.name);
  }
} else if(!printRequests) {
  print("skipped live data test: " + liveFile + " missing (run ./fetch-data.sh)");
}

if(failures) { throw new Error(failures + " FAILURES"); }
print("all passed");

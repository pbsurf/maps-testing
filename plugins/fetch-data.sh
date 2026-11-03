#!/bin/sh
# Download live responses for the plugin tests into data/ (not committed; tests skip what's missing).
# Runs the tests with PRINT_REQUESTS=1 to get the exact URLs and headers the plugins send, then fetches
# each one. Live data changes over time, so the tests only apply checks that hold for any data to it.
#   ./fetch-data.sh    (from this directory; needs curl and a built dukrun: run `make` first)
set -e
cd "$(dirname "$0")"
DUKRUN=../../build/plugin-tests/dukrun
[ -x "$DUKRUN" ] || { echo "build dukrun first: make"; exit 1; }
mkdir -p data

fetch() {  # fetch <test> <url substring> <file>
  line=$(PRINT_REQUESTS=1 "$DUKRUN" app-stubs.js $(ls ../../assets/plugins/*.js | sort) "$1" 2>/dev/null \
    | grep "^REQUEST" | grep -F "$2" | head -n 1)
  [ -n "$line" ] || { echo "no request matching $2 from $1"; exit 1; }
  url=$(printf '%s' "$line" | cut -f2)
  hdr=$(printf '%s' "$line" | cut -f3)
  echo "$3 <- ${url%%\?*}"
  curl -sSf --retry 2 -A "AscendMaps-plugin-tests/1.0" ${hdr:+-H "$hdr"} -o "$3.tmp" "$url"
  mv "$3.tmp" "$3"
}

fetch test-wikipedia-search.js "query.wikidata.org/sparql" data/wikidata-strasbourg.json

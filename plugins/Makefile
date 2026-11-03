# Tests for JS plugins in assets/plugins, run with duktape from tangram-es
# - each test-*.js runs in a fresh heap: dukrun app-stubs.js <all plugins> test-*.js
# - `make` builds dukrun and runs all tests; `make TESTS=test-foo.js` runs one; VERBOSE=1 for more output
# - live API responses for some tests go in data/ (not committed): ./fetch-data.sh downloads them; tests
#   skip the live part when they are missing

DUKTAPE = ../../tangram-es/core/deps/duktape
BUILDDIR = ../../build/plugin-tests
PLUGINDIR = ../../assets/plugins
# plugins are loaded alphabetically, as by PluginManager::reload()
PLUGINS = $(sort $(wildcard $(PLUGINDIR)/*.js))
TESTS ?= $(wildcard test-*.js)
DUKRUN = $(BUILDDIR)/dukrun

.PHONY: all test clean
all: test

$(DUKRUN): dukrun.c $(DUKTAPE)/duktape.c $(DUKTAPE)/duktape.h $(DUKTAPE)/duk_config.h
	@mkdir -p $(BUILDDIR)
	$(CC) -O1 -std=c99 -Wall -I$(DUKTAPE) dukrun.c $(DUKTAPE)/duktape.c -lm -o $@

test: $(DUKRUN)
	@fail=0; for t in $(TESTS); do \
	  echo "== $$t"; \
	  $(DUKRUN) app-stubs.js $(PLUGINS) $$t || fail=1; \
	done; exit $$fail

clean:
	rm -f $(DUKRUN)

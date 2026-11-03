# Linux makefile for the tangram benchmark replay (testing/bench/README.md); run from the explore root
# - make -f testing/bench.mk   builds build/Bench/replay
# - TANGRAM_BENCH enables the counters in tangram-es/core/src/debug/benchCounters.h
# - PROFILE=1 (default) keeps frame pointers and avoids AVX-512 for valgrind

TARGET ?= replay
DEBUG ?= 0
PROFILE ?= 1
BUILDDIR ?= build/Bench

include make/shared.mk

DEFS += LOG_LEVEL=2 TANGRAM_BENCH

## modules
include tangram-es/core/module.mk
include testing/bench/module.mk

# app implementations of tangram user functions (native style functions, SDF glyphs, SVG sprites) and
#  their dependencies, so the replay does the same work as the app
MODULE_BASE := .

MODULE_SOURCES = \
  app/src/stylefns.cpp \
  app/src/tangramfns.cpp \
  testing/bench/benchimpl.cpp \
  $(STYLUSLABS_DEPS)/ulib/geom.cpp \
  $(STYLUSLABS_DEPS)/ulib/image.cpp \
  $(STYLUSLABS_DEPS)/ulib/path2d.cpp \
  $(STYLUSLABS_DEPS)/ulib/painter.cpp \
  $(STYLUSLABS_DEPS)/usvg/svgnode.cpp \
  $(STYLUSLABS_DEPS)/usvg/svgstyleparser.cpp \
  $(STYLUSLABS_DEPS)/usvg/svgparser.cpp \
  $(STYLUSLABS_DEPS)/usvg/svgpainter.cpp \
  $(STYLUSLABS_DEPS)/usvg/cssparser.cpp \
  $(STYLUSLABS_DEPS)/nanovgXC/src/nanovg.c \
  $(STYLUSLABS_DEPS)/pugixml/src/pugixml.cpp

MODULE_INC_PRIVATE = $(STYLUSLABS_DEPS) $(STYLUSLABS_DEPS)/nanovgXC/src $(STYLUSLABS_DEPS)/pugixml/src app/src app/include
MODULE_DEFS_PRIVATE = NO_PAINTER_GL NO_PAINTER_SWU SVGGUI_NO_SDL PUGIXML_NO_XPATH PUGIXML_NO_EXCEPTIONS

include $(ADD_MODULE)

LIBS = -pthread -lcurl -ldl
DEFS += TANGRAM_LINUX

include make/unix.mk

.DEFAULT_GOAL := all

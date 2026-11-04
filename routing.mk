# Linux makefile for the offline router test driver (routing/routetest.cpp); run from the styluslabs/maps root
# - make -f testing/routing.mk   builds build/Routing/routetest
# - links tangram core for scene filters and YAML; the app code in offlinerouter.cpp is excluded

TARGET ?= routetest
DEBUG ?= 0
BUILDDIR ?= build/Routing

include make/shared.mk

DEFS += LOG_LEVEL=2

## modules
include tangram-es/core/module.mk

MODULE_BASE := .

MODULE_SOURCES = \
  testing/routing/routetest.cpp \
  tangram-es/tests/src/gl_mock.cpp

MODULE_INC_PRIVATE = tangram-es/tests/src

include $(ADD_MODULE)

LIBS = -pthread -ldl
DEFS += TANGRAM_LINUX

include make/unix.mk

.DEFAULT_GOAL := all

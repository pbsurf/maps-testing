# Tangram benchmark replay (see README.md); built by testing/bench.mk from the styluslabs/maps root

MAKE_BASE := $(GET_MAKE_BASE)

MODULE_BASE = $(MAKE_BASE)

MODULE_SOURCES = \
  src/replay.cpp \
  src/benchPlatform.cpp \
  ../../tangram-es/tests/src/gl_mock.cpp \
  ../../tangram-es/platforms/common/urlClient.cpp

MODULE_INC_PRIVATE = src ../../tangram-es/tests/src ../../tangram-es/platforms/common

include $(ADD_MODULE)

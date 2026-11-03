// Single-header library implementations for the benchmark replay (testing/bench.mk), which links
//  app/src/tangramfns.cpp and its usvg/ulib/nanovg dependencies without the rest of the app.
//  The app gets these from app/src/resources.cpp.

#include <stdio.h>
#include "nanovg.h"

#define FONTSTASH_IMPLEMENTATION
#include "fontstash.h"

#define NANOVG_SW_IMPLEMENTATION
#define NVGSW_QUIET_FRAME
#include "nanovgXC/src/nanovg_sw.h"

#define PLATFORMUTIL_IMPLEMENTATION
#include "ulib/platformutil.h"

#define STRINGUTIL_NO_STB_IMPL
#define STRINGUTIL_IMPLEMENTATION
#include "ulib/stringutil.h"

#define FILEUTIL_IMPLEMENTATION
#include "ulib/fileutil.h"

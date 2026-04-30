# Build the Interrogate native support library.  BUILD_SHARED_LIBS decides
# whether this is a shared or static artifact, matching the link:shared /
# link:static matrix axis used by .github/workflows/nuget.yml so each CI job
# produces exactly one of:
#   libinterrogate_support.so / .dylib    (BUILD_SHARED_LIBS=ON)
#   libinterrogate_support.a              (BUILD_SHARED_LIBS=OFF)
#   interrogate_support.dll               (Windows shared)
#   interrogate_support.lib               (Windows static)
#
# The two artifacts then get packaged together under runtimes/<rid>/native/
# inside the final Interrogate.Core.nupkg.

set(IG_SUPPORT_SOURCES
  streamBridge.cxx
)

set(IG_SUPPORT_HEADERS
  streamBridge.h
)

add_library(interrogate_support
  ${IG_SUPPORT_SOURCES} ${IG_SUPPORT_HEADERS})

target_include_directories(interrogate_support PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

set_target_properties(interrogate_support PROPERTIES
  CXX_STANDARD 17
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON)

# hakuX-Boosted benchmark rig: Linux ARM64, no Android. See
# boosted/notes/bench-harness.md for what it measures and what it cannot.
cmake_minimum_required(VERSION 3.22)
project(boosted_bench LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# The app's own flags (x360/emulator-core/build.gradle and its CMakeLists):
# -O2 -DNDEBUG -fno-strict-aliasing, armv8-a baseline (so atomics go through
# the outline helpers, as in libe.so), frame pointers kept.
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG -g1")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG -g1")
add_compile_options(-march=armv8-a -fno-strict-aliasing -fno-omit-frame-pointer
                    -funwind-tables)

# The tree whose core is measured. The default is the tree this directory is
# in; the workflow points a second build at another branch's sources.
set(BENCH_TREE "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "Root of the hakuX-Boosted tree to measure")
set(XE_ROOT "${BENCH_TREE}/x360/emulator-core/src/main/cpp/xenia")
set(XE_SRC "${XE_ROOT}/src/xenia")
set(XE_TP "${XE_ROOT}/third_party")

option(BENCH_PRIMITIVES "Host primitive microbenchmarks" ON)
option(BENCH_CORE "Benchmarks that build the Xbox 360 core's own sources" ON)

add_subdirectory(common)
if(BENCH_PRIMITIVES)
  add_subdirectory(prim)
endif()
if(BENCH_CORE)
  add_subdirectory(core)
endif()

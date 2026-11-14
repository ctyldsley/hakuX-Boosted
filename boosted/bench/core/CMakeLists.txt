# The Xbox 360 core's own sources, compiled for Linux ARM64 as one shared
# library so thread-locals are reached the way libe.so reaches them (-fPIC,
# TLS descriptors). Nothing here is copied: the files are globbed from the
# tree, with the same platform-suffix rule as xenia's xe_platform_sources.

function(bench_xe_sources out_var dir)
  file(GLOB _all CONFIGURE_DEPENDS "${dir}/*.cc" "${dir}/*.c")
  set(_keep)
  foreach(src ${_all})
    get_filename_component(_n ${src} NAME_WE)
    if(_n MATCHES "(_main|_test|_demo)$")
      continue()
    endif()
    if(_n MATCHES "(_win|_android|_mac|_amd64|_x64|_x11|_gtk)$")
      continue()
    endif()
    list(APPEND _keep ${src})
  endforeach()
  set(${out_var} ${_keep} PARENT_SCOPE)
endfunction()

bench_xe_sources(_base "${XE_SRC}/base")
list(FILTER _base EXCLUDE REGEX "(console_app_main_|main_init_|png_utils|embedded_bundle)")
bench_xe_sources(_cpu "${XE_SRC}/cpu")
bench_xe_sources(_cpu_backend "${XE_SRC}/cpu/backend")
bench_xe_sources(_cpu_compiler "${XE_SRC}/cpu/compiler")
bench_xe_sources(_cpu_passes "${XE_SRC}/cpu/compiler/passes")
bench_xe_sources(_cpu_hir "${XE_SRC}/cpu/hir")
bench_xe_sources(_cpu_ppc "${XE_SRC}/cpu/ppc")
bench_xe_sources(_a64 "${XE_SRC}/cpu/backend/a64")

# The guest scheduler and the kernel objects it schedules. kernel_state.cc is
# deliberately absent: see sched/kernel_shim.cc.
set(_kernel
  "${XE_SRC}/kernel/guest_scheduler.cc"
  "${XE_SRC}/kernel/xthread.cc"
  "${XE_SRC}/kernel/xobject.cc"
  "${XE_SRC}/kernel/xevent.cc"
  "${XE_SRC}/kernel/xsemaphore.cc"
  "${XE_SRC}/kernel/xmutant.cc"
  "${XE_SRC}/kernel/xtimer.cc"
  "${XE_SRC}/kernel/kernel_flags.cc"
  "${XE_SRC}/kernel/util/object_table.cc"
  "${XE_SRC}/kernel/util/native_list.cc"
  "${XE_SRC}/kernel/util/shim_utils.cc"
  # A header these all include builds a table of default profile settings at
  # start-up, so the type's constructors have to exist.
  "${XE_SRC}/kernel/xam/user_settings.cc"
  "${XE_SRC}/kernel/xam/user_data.cc"
  "${XE_SRC}/kernel/xboxkrnl/xboxkrnl_threading.cc"
)

set(_bc "${XE_TP}/boost_context/context/src/asm")
file(GLOB _capstone
  "${XE_TP}/capstone/*.c"
  "${XE_TP}/capstone/arch/AArch64/*.c")
file(GLOB _xbyak "${XE_TP}/xbyak_aarch64/src/*.cpp")

add_library(xecore SHARED
  ${_base} ${_cpu} ${_cpu_backend} ${_cpu_compiler} ${_cpu_passes} ${_cpu_hir}
  ${_cpu_ppc} ${_a64} ${_kernel}
  "${XE_SRC}/memory.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/bench_env.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/bench_xlat.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/bench_sched.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/core_main.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/kernel_shim.cc"
  "${_bc}/make_arm64_aapcs_elf_gas.S"
  "${_bc}/jump_arm64_aapcs_elf_gas.S"
  "${_bc}/ontop_arm64_aapcs_elf_gas.S"
  ${_capstone} ${_xbyak}
  "${XE_TP}/fmt/src/format.cc" "${XE_TP}/fmt/src/os.cc"
  "${XE_ROOT}/../aarch64_disasm.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/compat/bench_compat.c"
  "${XE_TP}/mspack/lzxd.c" "${XE_TP}/mspack/system.c" "${XE_TP}/mspack/logging.cc"
)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gen/version.h"
"#define XE_BUILD_BRANCH \"bench\"\n#define XE_BUILD_COMMIT \"0\"\n#define XE_BUILD_COMMIT_SHORT \"0\"\n#define XE_BUILD_DATE __DATE__\n")
target_include_directories(xecore PUBLIC
  "${XE_ROOT}" "${XE_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}/gen")
target_include_directories(xecore SYSTEM PUBLIC
  "${XE_TP}" "${XE_TP}/fmt/include" "${XE_TP}/llvm/include"
  "${XE_TP}/capstone/include" "${XE_TP}/xbyak_aarch64/xbyak_aarch64"
  "${XE_TP}/xbyak_aarch64" "${XE_TP}/mspack" "${XE_TP}/capstone")
target_compile_definitions(xecore PUBLIC
  _LIB USE_CPP17 CAPSTONE_HAS_ARM64 CAPSTONE_HAS_AARCH64 CAPSTONE_USE_SYS_DYN_MEM
  # The fork's own code paths (its JIT unwind table, its guest memory
  # mapping) hang off this; platform.h sets it only under __ANDROID__.
  XE_PLATFORM_xendroid=1)
target_compile_definitions(xecore PRIVATE HAVE_CONFIG_H)
target_include_directories(xecore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/compat")
target_compile_options(xecore PRIVATE -w
  "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/bench_compat.h>")
target_link_libraries(xecore PUBLIC benchcommon pthread dl rt)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/compat/bench_compat.c"
  PROPERTIES COMPILE_DEFINITIONS _GNU_SOURCE)

# The translator's counters are read if this tree has them under the names
# the harness knows; a tree that renamed them still builds and says so.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES "${XE_ROOT}/src")
set(CMAKE_REQUIRED_FLAGS "-std=c++20")
set(_bench_try_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)  # compile, do not link
check_cxx_source_compiles("
#include \"xenia/cpu/compiler/translation_stats.h\"
int main() {
  auto& s = xe::cpu::compiler::translation_stats;
  return int(s.pin_functions_considered.load() + s.pin_functions_pinned.load() +
             s.pin_functions_with_loops.load() +
             s.pin_verify_mismatches.load() + s.simplify_block_walks.load() +
             s.simplify_block_skips.load() + s.simplify_skip_mismatches.load());
}" BENCH_HAVE_TRANSLATION_STATS_COMPILES)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_FLAGS)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_bench_try_type}")
if(BENCH_HAVE_TRANSLATION_STATS_COMPILES)
  target_compile_definitions(xecore PRIVATE BENCH_HAVE_TRANSLATION_STATS=1)
else()
  target_compile_definitions(xecore PRIVATE BENCH_HAVE_TRANSLATION_STATS=0)
endif()

# The two ways of loading the library: see launcher.c.
add_executable(bench_core launcher.c)
set_target_properties(bench_core PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_link_libraries(bench_core PRIVATE dl)
add_dependencies(bench_core xecore)

add_executable(bench_core_static launcher.c)
set_target_properties(bench_core_static PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_compile_definitions(bench_core_static PRIVATE BENCH_STATIC)
# Found beside the executable, so the built files can be moved together.
set_target_properties(bench_core_static PROPERTIES
  BUILD_RPATH "$ORIGIN" BUILD_RPATH_USE_ORIGIN ON)
target_link_libraries(bench_core_static PRIVATE xecore)

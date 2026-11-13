# hakuX-Boosted: a small build of the PowerPC instruction tests for CI.
#
# The core's own build (the CMakeLists.txt five directories up) links every
# subsystem into the test runner: the GPU, audio, SDL, GTK. None of that is
# needed to translate and run a PowerPC instruction, and none of it builds
# quickly on a bare runner. This project compiles only what the translator
# needs (xenia-base, the CPU, the ARM64 back end, guest memory) with the
# stand-ins of stubs.cc for the few symbols of other subsystems they name,
# and links the same runner source, ppc_testing_main.cc.
#
#   cmake -S src/xenia/cpu/ppc/testing/ci -B build-ppc-tests -G Ninja \
#         -DCMAKE_BUILD_TYPE=Release
#   ninja -C build-ppc-tests
#
# ARM64 Linux only (natively, or cross-compiled and run under qemu-aarch64).
cmake_minimum_required(VERSION 3.20)
project(xenia_ppc_tests_ci LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

get_filename_component(XE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
set(XE_SRC "${XE_ROOT}/src/xenia")
set(XE_TP "${XE_ROOT}/third_party")

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  message(FATAL_ERROR "the instruction tests run the ARM64 back end: build for aarch64")
endif()

file(WRITE "${CMAKE_BINARY_DIR}/version.h"
"#define XE_BUILD_BRANCH \"ci\"\n#define XE_BUILD_COMMIT \"0\"\n#define XE_BUILD_COMMIT_SHORT \"0\"\n#define XE_BUILD_DATE __DATE__\n")

include_directories(${XE_ROOT} ${XE_ROOT}/src ${CMAKE_BINARY_DIR})
include_directories(SYSTEM ${XE_TP} ${XE_TP}/fmt/include
  ${XE_TP}/xbyak_aarch64 ${XE_TP}/xbyak_aarch64/src ${XE_TP}/xbyak_aarch64/xbyak_aarch64
  ${XE_TP}/capstone/include ${XE_TP}/mspack ${XE_TP}/llvm/include)
add_compile_definitions(_LIB USE_CPP17 CAPSTONE_HAS_ARM64 CAPSTONE_HAS_AARCH64
  CAPSTONE_USE_SYS_DYN_MEM $<$<CONFIG:Release>:NDEBUG>)
add_compile_options(-w -g1)

# Sources of one directory, without the other platforms' and the mains.
function(xe_sources out dir)
  file(GLOB _all "${dir}/*.cc" "${dir}/*.c")
  set(_keep)
  foreach(src ${_all})
    get_filename_component(_n ${src} NAME_WE)
    if(_n MATCHES "(_main|_test|_demo|_win|_mac|_android|_amd64|_x64|_x11|_gtk)$")
      continue()
    endif()
    if(_n MATCHES "^(console_app_main|main_init)_")
      continue()
    endif()
    list(APPEND _keep ${src})
  endforeach()
  set(${out} ${_keep} PARENT_SCOPE)
endfunction()

xe_sources(_base ${XE_SRC}/base)
list(FILTER _base EXCLUDE REGEX "/(png_utils|embedded_bundle|profiling)\\.cc$")
xe_sources(_cpu ${XE_SRC}/cpu)
list(FILTER _cpu EXCLUDE REGEX "/(xex_module|elf_module|lzx)\\.cc$")
xe_sources(_cpu_backend ${XE_SRC}/cpu/backend)
xe_sources(_cpu_compiler ${XE_SRC}/cpu/compiler)
xe_sources(_cpu_passes ${XE_SRC}/cpu/compiler/passes)
xe_sources(_cpu_hir ${XE_SRC}/cpu/hir)
xe_sources(_cpu_ppc ${XE_SRC}/cpu/ppc)
xe_sources(_cpu_a64 ${XE_SRC}/cpu/backend/a64)

file(GLOB _capstone_arch "${XE_TP}/capstone/arch/AArch64/*.c")
set(_third_party
  ${XE_TP}/fmt/src/format.cc
  ${XE_TP}/fmt/src/os.cc
  ${XE_TP}/xbyak_aarch64/src/xbyak_aarch64_impl.cpp
  ${XE_TP}/xbyak_aarch64/src/util_impl.cpp
  ${XE_TP}/capstone/cs.c ${XE_TP}/capstone/MCInst.c ${XE_TP}/capstone/MCInstPrinter.c
  ${XE_TP}/capstone/MCInstrDesc.c ${XE_TP}/capstone/MCRegisterInfo.c
  ${XE_TP}/capstone/SStream.c ${XE_TP}/capstone/utils.c ${XE_TP}/capstone/Mapping.c
  ${_capstone_arch}
  ${XE_TP}/boost_context/context/src/asm/make_arm64_aapcs_elf_gas.S
  ${XE_TP}/boost_context/context/src/asm/jump_arm64_aapcs_elf_gas.S
  ${XE_TP}/boost_context/context/src/asm/ontop_arm64_aapcs_elf_gas.S
)

add_executable(xenia-cpu-ppc-tests
  ${XE_SRC}/cpu/ppc/testing/ppc_testing_main.cc
  ${XE_SRC}/base/console_app_main_posix.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs.cc
  ${XE_SRC}/memory.cc
  ${_base} ${_cpu} ${_cpu_backend} ${_cpu_compiler} ${_cpu_passes} ${_cpu_hir}
  ${_cpu_ppc} ${_cpu_a64} ${_third_party}
)
target_include_directories(xenia-cpu-ppc-tests PRIVATE ${XE_TP}/capstone)
# The code cache's unwind tables exist in one form only, the fork's own
# (XE_PLATFORM_xendroid, a define platform.h otherwise makes on Android):
# these two files are compiled as the phone's build compiles them.
set_source_files_properties(
  ${XE_SRC}/cpu/backend/a64/a64_code_cache.cc
  ${XE_SRC}/cpu/backend/a64/a64_code_cache_posix.cc
  PROPERTIES COMPILE_DEFINITIONS "XE_PLATFORM_xendroid=1")
target_sources(xenia-cpu-ppc-tests PRIVATE ${XE_ROOT}/../aarch64_disasm.cpp)
target_compile_definitions(xenia-cpu-ppc-tests PRIVATE XE_SOURCE_ROOT="${XE_ROOT}")
target_link_libraries(xenia-cpu-ppc-tests PRIVATE pthread dl rt m)

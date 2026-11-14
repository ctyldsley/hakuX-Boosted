# Host primitive microbenchmarks. Needs nothing from the core but the three
# fcontext assembly files.

# The library the app's way: -fPIC, TLS descriptors, loaded with dlopen. The
# pad makes its thread-local block larger than glibc's spare static TLS, so
# the loader has to bind the dynamic resolver, as bionic always does.
add_library(prim_tls_dyn SHARED tls_lib.c)
target_compile_definitions(prim_tls_dyn PRIVATE
  PRIM_PREFIX=prim_tls_dyn PRIM_TLS_PAD=65536)

# The same source loaded at start-up: the loader's static resolver.
add_library(prim_tls_static SHARED tls_lib.c)
target_compile_definitions(prim_tls_static PRIVATE PRIM_PREFIX=prim_tls_static)

add_library(prim_atomics_outline OBJECT atomics.cc)
target_compile_definitions(prim_atomics_outline PRIVATE PRIM_PREFIX=prim_outline)
target_compile_options(prim_atomics_outline PRIVATE -march=armv8-a -moutline-atomics)

add_library(prim_atomics_lse OBJECT atomics.cc)
target_compile_definitions(prim_atomics_lse PRIVATE PRIM_PREFIX=prim_lse)
target_compile_options(prim_atomics_lse PRIVATE -march=armv8.1-a -mno-outline-atomics)

add_library(prim_atomics_llsc OBJECT atomics.cc)
target_compile_definitions(prim_atomics_llsc PRIVATE PRIM_PREFIX=prim_llsc)
target_compile_options(prim_atomics_llsc PRIVATE -march=armv8-a -mno-outline-atomics)

set(_bc "${XE_TP}/boost_context/context/src/asm")
add_executable(bench_prim
  prim_main.cc
  $<TARGET_OBJECTS:prim_atomics_outline>
  $<TARGET_OBJECTS:prim_atomics_lse>
  $<TARGET_OBJECTS:prim_atomics_llsc>
  "${_bc}/make_arm64_aapcs_elf_gas.S"
  "${_bc}/jump_arm64_aapcs_elf_gas.S"
  "${_bc}/ontop_arm64_aapcs_elf_gas.S"
)
# The executable itself is not position independent and has exactly one
# thread-local of its own first in line (prim_slot_app): see prim_main.cc.
set_target_properties(bench_prim PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_link_libraries(bench_prim PRIVATE benchcommon prim_tls_static dl pthread)
set_target_properties(bench_prim PROPERTIES
  BUILD_RPATH "$ORIGIN" BUILD_RPATH_USE_ORIGIN ON)
add_dependencies(bench_prim prim_tls_dyn)

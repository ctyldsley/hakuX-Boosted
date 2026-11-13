add_library(xenia-gpu STATIC)
xe_platform_sources(xenia-gpu ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(xenia-gpu PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/glslang
  ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
)
target_link_libraries(xenia-gpu PUBLIC dxbc fmt snappy xxhash xenia-base xenia-ui)
target_link_libraries(xenia-gpu PRIVATE glslang-spirv)
# hakuX-Boosted: the SPIRV-Tools optimiser library, for the optional pass over
# translated guest shaders (the spirv_optimize setting,
# spirv_shader_translator_optimize.cc). third_party adds SPIRV-Tools, excluded
# from "all", for the host shader tool; linking the library here is what makes
# it part of the Android build. Where third_party leaves SPIRV-Tools out the
# pass compiles to a stub. Its headers as SYSTEM so that their warnings stay
# out of this target's -Werror.
if(TARGET SPIRV-Tools-opt)
  target_link_libraries(xenia-gpu PRIVATE SPIRV-Tools-opt)
  target_include_directories(xenia-gpu SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/SPIRV-Tools/include)
  target_compile_definitions(xenia-gpu PRIVATE XE_GPU_SPIRV_TOOLS_OPT=1)
endif()
xe_target_defaults(xenia-gpu)

if(XENIA_BUILD_MISC)
  # Shader compiler tool
  add_executable(xenia-gpu-shader-compiler
    ${CMAKE_CURRENT_SOURCE_DIR}/shader_compiler_main.cc
  )
  if(WIN32)
    target_sources(xenia-gpu-shader-compiler PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_win.cc)
  else()
    target_sources(xenia-gpu-shader-compiler PRIVATE
      ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_posix.cc)
  endif()
  target_include_directories(xenia-gpu-shader-compiler PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
    ${PROJECT_SOURCE_DIR}/third_party/glslang
  )
  target_link_libraries(xenia-gpu-shader-compiler PRIVATE
    dxbc fmt glslang-spirv snappy xenia-base xenia-gpu xenia-ui xenia-ui-vulkan
  )
  xe_target_defaults(xenia-gpu-shader-compiler)
endif()

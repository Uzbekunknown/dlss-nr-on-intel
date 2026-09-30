# Adapted from andyvand/dlss-nr-on-vulkan, commit
# 1519a7a9035a94bc40f2d45d52caafe5a2ad9800 (Apache-2.0; see NOTICE).
# This adaptation builds the current Linux/XMX sources. The fork's portable GEMM,
# C frame library and macOS/Windows runtime changes have not been imported.
cmake_minimum_required(VERSION 3.21)
project(dlss_nr_on_intel LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "This source tree currently supports Linux. CMake alone does not add the fork's macOS/Windows runtime support.")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B work/cmake")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
option(NR_BUILD_LAYER "Build the Vulkan game layer and its tests" ON)
option(NR_BUILD_TESTS "Register checks with CTest" ON)

# Python and the launchers currently find artifacts in source/work. Keep that
# contract explicit: do not expose an output-directory knob they cannot follow.
set(NR_WORK_DIR "${PROJECT_SOURCE_DIR}/work")
file(MAKE_DIRECTORY "${NR_WORK_DIR}")
find_package(Vulkan QUIET)
find_path(NR_VULKAN_INCLUDE vulkan/vulkan.h
  HINTS ${Vulkan_INCLUDE_DIRS} "${NR_WORK_DIR}/vulkan-headers/include"
        "$ENV{VULKAN_SDK}/include" REQUIRED)
find_library(NR_VULKAN_LOADER NAMES vulkan
  HINTS "$ENV{VULKAN_SDK}/lib" REQUIRED)
find_program(NR_GLSLANG NAMES glslangValidator
  HINTS "$ENV{VULKAN_SDK}/bin" REQUIRED)
add_library(nr_vulkan INTERFACE)
target_include_directories(nr_vulkan INTERFACE "${NR_VULKAN_INCLUDE}")
target_link_libraries(nr_vulkan INTERFACE "${NR_VULKAN_LOADER}")

function(nr_output target)
  set_target_properties(${target} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${NR_WORK_DIR}/$<0:>"
    RUNTIME_OUTPUT_DIRECTORY "${NR_WORK_DIR}/$<0:>")
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
endfunction()

# Every shader the runtime loads, with the Makefile's defines; `src/tools/build_check.py`
# fails the tests if the two lists part. Every shader depends on every shared GLSL
# include: a superset, so an edit to one rebuilds a few shaders too many, never too few.
set(NR_SPV)
set(NR_GLSL_INCLUDES publish specialize residual_epilogue cosine_tree qkv_epilogue)
list(TRANSFORM NR_GLSL_INCLUDES PREPEND "${PROJECT_SOURCE_DIR}/src/gpu/")
list(TRANSFORM NR_GLSL_INCLUDES APPEND ".glsl")
function(nr_shader output source)
  set(out "${NR_WORK_DIR}/${output}")
  add_custom_command(OUTPUT "${out}"
    COMMAND "${NR_GLSLANG}" --target-env vulkan1.3
      "-I${PROJECT_SOURCE_DIR}/src/gpu" ${ARGN} -o "${out}" "${PROJECT_SOURCE_DIR}/${source}"
    DEPENDS "${PROJECT_SOURCE_DIR}/${source}" ${NR_GLSL_INCLUDES}
    VERBATIM)
  set(NR_SPV ${NR_SPV} "${out}" PARENT_SCOPE)
endfunction()
nr_shader(gemm_resident.spv src/gpu/gemm_resident.comp)
nr_shader(gemm_tiled.spv src/gpu/gemm_resident.comp -DRM=2 -DRN=2)
nr_shader(gemm_staged.spv src/gpu/gemm_staged.comp)
nr_shader(gemm_staged32.spv src/gpu/gemm_staged.comp -DSTAGED_BM=32)
nr_shader(gemm_staged32_deep.spv src/gpu/gemm_staged.comp -DSTAGED_BM=32 -DSTAGED_BK=64)
nr_shader(attention_rows.spv src/gpu/attention.comp -DROW_LANES=256)
nr_shader(resident.spv src/gpu/resident.comp)
nr_shader(attention.spv src/gpu/attention.comp)
nr_shader(history.spv src/gpu/history.comp)
nr_shader(gemm_coopmat.spv src/gpu/gemm_coopmat.comp)
nr_shader(gemm_batched.spv src/gpu/gemm_coopmat_batched.comp)
nr_shader(gemm_f16acc.spv src/gpu/gemm_coopmat_f16acc.comp)
nr_shader(gemm_coopmat_int8.spv src/gpu/gemm_coopmat_int8.comp)
nr_shader(gemm_staged_int8.spv src/gpu/gemm_staged_int8.comp)
nr_shader(window_attention.spv src/gpu/window_attention.comp)
nr_shader(window_block.spv src/gpu/window_block.comp)
nr_shader(global_attention.spv src/gpu/global_attention.comp)
nr_shader(ffn_fused.spv src/gpu/ffn_fused.comp)
if(NR_BUILD_TESTS)
  nr_shader(attention_ab.spv src/gpu/attention.comp -DSOFTMAX_AB)
endif()
add_custom_target(shaders ALL DEPENDS ${NR_SPV})

add_library(xmx SHARED src/gpu/libxmx.c)
target_link_libraries(xmx PRIVATE nr_vulkan)
target_compile_options(xmx PRIVATE -O2)
nr_output(xmx)
add_executable(gemm_runner src/gpu/gemm_runner.c)
target_link_libraries(gemm_runner PRIVATE nr_vulkan)
target_compile_options(gemm_runner PRIVATE -O2)
nr_output(gemm_runner)

# The Makefile's flags, for the Makefile's reasons: no contraction and no fast maths keep
# the bytes the NumPy's; -fno-trapping-math lets the fused composition vectorise; OpenMP
# splits every pass by rows.
find_package(OpenMP REQUIRED COMPONENTS C)
add_library(nr_image SHARED src/ref/nr_image.c)
target_compile_options(nr_image PRIVATE -O3 -ffp-contract=off -fno-fast-math
  -fno-trapping-math)
target_link_libraries(nr_image PRIVATE OpenMP::OpenMP_C m)
include(CheckCCompilerFlag)
if(NOT CMAKE_CROSSCOMPILING)
  check_c_compiler_flag(-march=native NR_HAS_MARCH_NATIVE)
  if(NR_HAS_MARCH_NATIVE)
    target_compile_options(nr_image PRIVATE -march=native)
  endif()
endif()
nr_output(nr_image)

if(NR_BUILD_LAYER)
  find_package(Threads REQUIRED)
  add_library(nr_layer SHARED src/layer/nr_layer.c)
  target_link_libraries(nr_layer PRIVATE nr_vulkan Threads::Threads)
  target_compile_options(nr_layer PRIVATE -O2)
  nr_output(nr_layer)
  if(NR_BUILD_TESTS)
    foreach(test test_layer_loader test_present test_settled test_exchange)
      add_executable(${test} src/layer/${test}.c)
      target_link_libraries(${test} PRIVATE nr_vulkan Threads::Threads)
      target_compile_options(${test} PRIVATE -O2 -UNDEBUG)
      nr_output(${test})
    endforeach()
  endif()
endif()

if(NR_BUILD_TESTS)
  enable_testing()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  function(nr_test name)
    add_test(NAME ${name} COMMAND ${ARGN})
    # GPU tests share one device; concurrent processes distort timing and can exhaust
    # the iGPU's memory. Several layer tests also drive temporary daemon processes.
    set_tests_properties(${name} PROPERTIES RUN_SERIAL TRUE
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
  endfunction()
  if(NR_BUILD_LAYER)
    nr_test(settled "$<TARGET_FILE:test_settled>")
    foreach(name daemon ui_mask present temporal toggle panel)
      nr_test(layer_${name} "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/layer/test_${name}.py")
    endforeach()
    nr_test(layer_present_negative "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/src/layer/test_present.py" --negative-control)
    set_tests_properties(layer_present_negative PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  foreach(name publish_check claims_check build_check)
    nr_test(${name} "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/tools/${name}.py")
  endforeach()
  nr_test(profile_tables "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/bench/frame_profile.py" --tables)
  foreach(name gemm_contract input_fp16 compact_head joint_qkv gemm_int8 gemm_int8_staged
      window_block global_attention int8_quant gemm_residual window_residual window_attention
      gemm_qkv glue ffn_fused staged_partial staged32 epilogue specialization softmax_pack
      qkv_fusion scratch_arena graph frame_execution resident)
    nr_test(gpu_${name} "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/gpu/test_${name}.py")
  endforeach()
  nr_test(gpu_ffn_batch "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/gpu/test_ffn_batch.py" --gpu)
  foreach(name frame_cache native_image nr_model temporal_controls)
    nr_test(ref_${name} "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/ref/test_${name}.py")
  endforeach()
endif()
message(STATUS "Linux/XMX build; artifacts: ${NR_WORK_DIR}; layer: ${NR_BUILD_LAYER}")

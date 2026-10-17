# Adapted from andyvand/dlss-nr-on-vulkan, commit
# 1519a7a9035a94bc40f2d45d52caafe5a2ad9800 (Apache-2.0; see NOTICE).
# This adaptation builds the current XMX sources on Linux, and on the `windows` branch the
# compute side on Windows too — the runtime, the native passes, the probe; the game layer is
# not ported there yet (docs/WINDOWS.md). The fork's portable GEMM and C frame library have
# not been imported.
cmake_minimum_required(VERSION 3.21)
project(dlss_nr_on_intel LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT WIN32)
  message(FATAL_ERROR "This source tree supports Linux, and Windows on the windows branch.")
endif()
if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "MSVC")
  message(FATAL_ERROR "The native passes need _Float16, which MSVC lacks: build with MSYS2's "
                      "UCRT64 gcc (12 or newer) or with clang (docs/WINDOWS.md).")
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
if(WIN32)
  set(NR_LAYER_DEFAULT OFF)   # POSIX sockets and threads: not ported yet
else()
  set(NR_LAYER_DEFAULT ON)
endif()
option(NR_BUILD_LAYER "Build the Vulkan game layer and its tests" ${NR_LAYER_DEFAULT})
option(NR_BUILD_TESTS "Register checks with CTest" ON)
option(NR_ENABLE_GPU_SMOKE_TESTS
  "Register manual Win32 GPU interop/present tests (run outside the sandbox with an exclusive GPU slot)" OFF)

# Python and the launchers currently find artifacts in source/work. Keep that
# contract explicit: do not expose an output-directory knob they cannot follow.
set(NR_WORK_DIR "${PROJECT_SOURCE_DIR}/work")
file(MAKE_DIRECTORY "${NR_WORK_DIR}")
find_package(Vulkan QUIET)
find_path(NR_VULKAN_INCLUDE vulkan/vulkan.h
  HINTS ${Vulkan_INCLUDE_DIRS} "${NR_WORK_DIR}/vulkan-headers/include"
        "$ENV{VULKAN_SDK}/include" REQUIRED)
find_library(NR_VULKAN_LOADER NAMES vulkan vulkan-1
  HINTS "$ENV{VULKAN_SDK}/lib" "$ENV{VULKAN_SDK}/Lib" REQUIRED)
find_program(NR_GLSLANG NAMES glslangValidator
  HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin" REQUIRED)
add_library(nr_vulkan INTERFACE)
target_include_directories(nr_vulkan INTERFACE "${NR_VULKAN_INCLUDE}")
target_link_libraries(nr_vulkan INTERFACE "${NR_VULKAN_LOADER}")

function(nr_output target)
  set_target_properties(${target} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${NR_WORK_DIR}/$<0:>"
    RUNTIME_OUTPUT_DIRECTORY "${NR_WORK_DIR}/$<0:>")
  get_target_property(type ${target} TYPE)
  if(WIN32 AND type STREQUAL "SHARED_LIBRARY")
    # the name Python asks for there: work/lib<name>.dll (src/gpu/xmx.py, native_library)
    set_target_properties(${target} PROPERTIES PREFIX "lib" SUFFIX ".dll")
  endif()
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
nr_shader(image_preprocess.spv src/gpu/image_preprocess.comp)
nr_shader(image_compose.spv src/gpu/image_compose.comp)
nr_shader(image_stats.spv src/gpu/image_stats.comp)
nr_shader(half_probe.spv src/bench/half_probe.comp)
if(NR_BUILD_TESTS)
  nr_shader(attention_ab.spv src/gpu/attention.comp -DSOFTMAX_AB)
endif()
add_custom_target(shaders ALL DEPENDS ${NR_SPV})

add_library(xmx SHARED src/gpu/libxmx.c)
target_link_libraries(xmx PRIVATE nr_vulkan)
target_compile_options(xmx PRIVATE -O2)
nr_output(xmx)
# What the GPU's cooperative matrix offers — the first thing to run on a new machine or driver.
add_executable(coopmat_probe src/probe/coopmat_probe.c)
target_link_libraries(coopmat_probe PRIVATE nr_vulkan)
nr_output(coopmat_probe)
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
target_link_libraries(nr_image PRIVATE OpenMP::OpenMP_C)
if(UNIX)
  target_link_libraries(nr_image PRIVATE m)
endif()
# The Makefile's CPU floor, NR_IMAGE_ARCH: x86-64-v3, so F16C makes each half conversion one
# instruction instead of a call into libgcc, and AVX2 is there for the loops.
include(CheckCCompilerFlag)
check_c_compiler_flag(-march=x86-64-v3 NR_HAS_X86_64_V3)
if(NR_HAS_X86_64_V3)
  target_compile_options(nr_image PRIVATE -march=x86-64-v3)
else()
  message(WARNING "the compiler takes no -march=x86-64-v3: the host passes will convert "
                  "every half through libgcc, several times slower")
endif()
nr_output(nr_image)

# The daemon's NumPy allocator on Windows, whose heap returns a frame's large blocks to the
# system for the next frame to fault in again (src/layer/nr_alloc.py).
if(WIN32)
  add_library(nr_alloc SHARED src/layer/nr_alloc.c)
  target_compile_options(nr_alloc PRIVATE -O2)
  nr_output(nr_alloc)
endif()

if(NR_BUILD_LAYER)
  find_package(Threads REQUIRED)
  add_library(nr_layer SHARED src/layer/nr_layer.c)
  target_link_libraries(nr_layer PRIVATE nr_vulkan Threads::Threads)
  target_compile_options(nr_layer PRIVATE -O2)
  nr_output(nr_layer)
  # The 32-bit layer, for 32-bit games under Proton, wherever a 32-bit compiler and a 32-bit
  # Vulkan loader are installed: the Makefile's LAYER32, with NR_BUILD_LAYER32 to override.
  if(NOT WIN32)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS -m32)
    set(CMAKE_REQUIRED_LINK_OPTIONS -m32)
    set(CMAKE_REQUIRED_LIBRARIES vulkan)
    check_c_source_compiles("int main(void) { return 0; }" NR_HAVE_VULKAN32)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    unset(CMAKE_REQUIRED_LIBRARIES)
    option(NR_BUILD_LAYER32 "Build the 32-bit layer too, for 32-bit games under Proton"
           ${NR_HAVE_VULKAN32})
    if(NR_BUILD_LAYER32)
      add_library(nr_layer32 SHARED src/layer/nr_layer.c)
      target_include_directories(nr_layer32 PRIVATE "${NR_VULKAN_INCLUDE}")
      target_compile_options(nr_layer32 PRIVATE -O2 -m32)
      target_link_options(nr_layer32 PRIVATE -m32)
      target_link_libraries(nr_layer32 PRIVATE vulkan)
      nr_output(nr_layer32)
    endif()
  endif()
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
  # Policy/transfer lifetime checks use mocked dispatch, so they are useful even
  # when the game layer itself is not built (the Windows compute configuration).
  find_package(Threads REQUIRED)
  foreach(test test_surface_policy test_transfer_resources)
    add_executable(${test} src/layer/${test}.c)
    target_link_libraries(${test} PRIVATE nr_vulkan Threads::Threads)
    target_compile_options(${test} PRIVATE -O2 -UNDEBUG)
    nr_output(${test})
  endforeach()
  if(WIN32)
    foreach(test test_gpu_bridge test_gpu_rpc test_gpu_rpc_native test_device_teardown test_gpu_only)
      add_executable(${test} src/layer/${test}.c)
      target_link_libraries(${test} PRIVATE nr_vulkan)
      target_compile_options(${test} PRIVATE -O2 -UNDEBUG)
      nr_output(${test})
    endforeach()
    # These executables are owned diagnostic fixtures, never installed games.
    # Building a fixture builds its runtime prerequisites, but does not run it.
    add_executable(test_gpu_interop src/layer/test_gpu_interop.c)
    target_link_libraries(test_gpu_interop PRIVATE nr_vulkan)
    target_compile_options(test_gpu_interop PRIVATE -O2 -UNDEBUG)
    add_dependencies(test_gpu_interop xmx shaders)
    nr_output(test_gpu_interop)
    if(NR_BUILD_LAYER)
      add_executable(test_gpu_present src/layer/test_gpu_present.c)
      target_link_libraries(test_gpu_present PRIVATE nr_vulkan user32)
      target_compile_options(test_gpu_present PRIVATE -O2 -UNDEBUG)
      add_dependencies(test_gpu_present nr_layer xmx nr_image nr_alloc shaders)
      nr_output(test_gpu_present)
    endif()
  endif()
  function(nr_test name)
    add_test(NAME ${name} COMMAND ${ARGN})
    # GPU tests share one device; concurrent processes distort timing and can exhaust
    # the iGPU's memory. Several layer tests also drive temporary daemon processes.
    set_tests_properties(${name} PROPERTIES RUN_SERIAL TRUE
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    if(WIN32)
      # Windows' Python opens text in the ANSI code page, and the notes and the README the
      # checks read are UTF-8.
      set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT "PYTHONUTF8=1")
    endif()
  endfunction()
  foreach(name surface_policy transfer_resources)
    nr_test(layer_${name} "$<TARGET_FILE:test_${name}>")
    set_tests_properties(layer_${name} PROPERTIES LABELS "host")
  endforeach()
  nr_test(layer_daemon_owner "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/layer/test_daemon_owner.py")
  nr_test(layer_gpu_transport "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/layer/test_gpu_transport.py")
  nr_test(layer_gpu_only_daemon "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/layer/test_gpu_only.py")
  set_tests_properties(layer_daemon_owner layer_gpu_transport layer_gpu_only_daemon PROPERTIES LABELS "host")
  if(WIN32)
    foreach(name gpu_bridge gpu_rpc gpu_rpc_native device_teardown gpu_only)
      nr_test(layer_${name} "$<TARGET_FILE:test_${name}>")
      set_tests_properties(layer_${name} PROPERTIES LABELS "host")
    endforeach()
    if(NR_ENABLE_GPU_SMOKE_TESTS)
      # Opt-in inventory only: an ordinary unfiltered CTest run must not open
      # Win32 WSI or contend with another GPU process. REQUIRED_FILES also
      # prevents the Python harness launching before the prerequisites exist.
      get_filename_component(NR_COMPILER_BIN "${CMAKE_C_COMPILER}" DIRECTORY)
      nr_test(gpu_win32_interop "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/src/layer/test_gpu_interop.py"
        --executable "$<TARGET_FILE:test_gpu_interop>")
      set_tests_properties(gpu_win32_interop PROPERTIES LABELS "gpu;manual;win32"
        TIMEOUT 90
        REQUIRED_FILES "$<TARGET_FILE:test_gpu_interop>;$<TARGET_FILE:xmx>;${NR_SPV}")
      set_property(TEST gpu_win32_interop APPEND PROPERTY ENVIRONMENT
        "NR_ROOT=${PROJECT_SOURCE_DIR}" "NR_DLL_PATH=${NR_COMPILER_BIN}")
      if(NR_BUILD_LAYER)
        nr_test(gpu_win32_present "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/src/layer/test_gpu_present.py" --run --sync-validation --exercise-controls
          --fixture "$<TARGET_FILE:test_gpu_present>" --layer "$<TARGET_FILE:nr_layer>")
        set_tests_properties(gpu_win32_present PROPERTIES LABELS "gpu;manual;win32"
          TIMEOUT 180
          REQUIRED_FILES "$<TARGET_FILE:test_gpu_present>;$<TARGET_FILE:nr_layer>;$<TARGET_FILE:xmx>;$<TARGET_FILE:nr_image>;$<TARGET_FILE:nr_alloc>;${NR_SPV};${NR_WORK_DIR}/mlxw/dlssnr-logical.safetensors")
        set_property(TEST gpu_win32_present APPEND PROPERTY ENVIRONMENT
          "NR_ROOT=${PROJECT_SOURCE_DIR}" "NR_DLL_PATH=${NR_COMPILER_BIN}")
      endif()
    endif()
  endif()
  if(NR_BUILD_LAYER)
    nr_test(settled "$<TARGET_FILE:test_settled>")
    foreach(name daemon ui_mask present temporal toggle panel)
      nr_test(layer_${name} "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/layer/test_${name}.py")
    endforeach()
    nr_test(layer_present_negative "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/src/layer/test_present.py" --negative-control)
    set_tests_properties(layer_present_negative PROPERTIES SKIP_RETURN_CODE 77)
    # The ImGui panel's own two fixtures. Windows-only by construction - they link ImGui,
    # GetAsyncKeyState and user32 - and off Windows the runner says so rather than skipping,
    # because nothing else watches what the panel reports. The executables come from
    # tools/build_win.bat, so REQUIRED_FILES keeps CTest from running before they exist.
    if(WIN32)
      nr_test(layer_menu_ui_fixtures "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/src/layer/test_menu_ui_fixtures.py")
      set_tests_properties(layer_menu_ui_fixtures PROPERTIES
        REQUIRED_FILES "${NR_WORK_DIR}/test_menu_panel.exe;${NR_WORK_DIR}/test_menu_side.exe")
    endif()
  endif()
  foreach(name publish_check claims_check build_check)
    nr_test(${name} "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/tools/${name}.py")
  endforeach()
  nr_test(release "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/tools/test_release.py")
  nr_test(setup_proxy "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/tools/test_setup_proxy.py")
  nr_test(profile_tables "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/bench/frame_profile.py" --tables)
  foreach(name device_pick gemm_contract denorm input_fp16 compact_head joint_qkv gemm_int8 gemm_int8_staged
      window_block global_attention int8_quant gemm_residual window_residual window_attention
      gemm_qkv glue ffn_fused staged_partial staged32 staged_packed epilogue specialization softmax_pack
      qkv_fusion scratch_arena graph frame_execution resident)
    nr_test(gpu_${name} "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/gpu/test_${name}.py")
  endforeach()
  # This diagnostic exercises the unmerged kernel the production graph does not
  # use. Intel's Windows driver hangs on it (docs/WINDOWS.md, phase71); keep it
  # visible in the inventory but prevent an unfiltered ctest from running it.
  if(WIN32)
    set_tests_properties(gpu_window_attention PROPERTIES DISABLED TRUE)
  endif()
  foreach(name image_preprocess image_compose image_stats)
    nr_test(gpu_${name} "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/gpu/test_${name}.py")
    set_tests_properties(gpu_${name} PROPERTIES LABELS "gpu;image")
  endforeach()
  nr_test(gpu_image_pipeline "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/gpu/test_image_pipeline.py" --full-model)
  set_tests_properties(gpu_image_pipeline PROPERTIES LABELS "gpu;image")
  nr_test(host_image_preprocess "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/gpu/test_image_preprocess.py" --cpu-only)
  foreach(name image_compose image_stats image_pipeline)
    nr_test(host_${name} "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/gpu/test_${name}.py" --host-only)
  endforeach()
  set_tests_properties(host_image_preprocess host_image_compose host_image_stats host_image_pipeline
    PROPERTIES LABELS "host;image")
  nr_test(gpu_ffn_batch "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/gpu/test_ffn_batch.py" --gpu)
  foreach(name frame_cache native_image nr_model temporal_controls dump_raw dump_geometry)
    nr_test(ref_${name} "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/ref/test_${name}.py")
  endforeach()
  # The daemon ends with its game on both systems: a process handle there, a pidfd here.
  nr_test(layer_spawn_exit "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/layer/test_spawn_exit.py")
  if(WIN32)
    foreach(name alloc pipe spawn_reasons vulkan_proxy)
      nr_test(layer_${name} "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/src/layer/test_${name}.py")
      set_tests_properties(layer_${name} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
  endif()
endif()
message(STATUS "${CMAKE_SYSTEM_NAME}/XMX build; artifacts: ${NR_WORK_DIR}; layer: ${NR_BUILD_LAYER}")

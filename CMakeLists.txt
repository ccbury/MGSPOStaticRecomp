cmake_minimum_required(VERSION 3.16)
project(mgspo_recomp LANGUAGES CXX)

if(WIN32 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR
    "MGSPO requires a 64-bit Windows build. Use an x64 Native Tools Command Prompt "
    "(or -A x64 with the Visual Studio generator) and configure in a fresh build directory.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra)
endif()

option(MGSPO_WATCH "Compile in guest store watchpoints (--watch)" OFF)
option(MGSPO_PROFILE_SYMBOLS "Add MSVC symbols for the sampling profiler" OFF)
if(MGSPO_WATCH)
  add_compile_definitions(MGSPO_WATCH)
endif()

add_library(mgspo STATIC
  src/iso.cpp
  src/sfo.cpp
  src/bootfile.cpp
  src/elf.cpp
  src/mips.cpp
  src/prx.cpp
  src/zar.cpp
  src/functions.cpp
  src/emit.cpp
  src/vfpu.cpp
)
target_include_directories(mgspo PUBLIC src runtime/include)

add_executable(mgspo-extract tools/extract/main.cpp)
target_link_libraries(mgspo-extract PRIVATE mgspo)

add_executable(mgspo-lift tools/lift/main.cpp)
target_link_libraries(mgspo-lift PRIVATE mgspo)

# One-step setup and launcher (Windows only): extract, lift, build and run from an ISO.
if(WIN32)
  set(MGSPO_TOOL_REVISION "unversioned")
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse HEAD
                    OUTPUT_VARIABLE MGSPO_GIT_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
                    RESULT_VARIABLE MGSPO_GIT_RESULT)
    if(MGSPO_GIT_RESULT EQUAL 0)
      set(MGSPO_TOOL_REVISION "${MGSPO_GIT_REVISION}")
    endif()
  endif()
  add_executable(mgspo-launcher tools/launcher/main.cpp)
  target_compile_definitions(mgspo-launcher PRIVATE MGSPO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                                                   MGSPO_TOOL_REVISION="${MGSPO_TOOL_REVISION}")
  target_link_libraries(mgspo-launcher PRIVATE comdlg32)
  if(MSVC)
    target_compile_options(mgspo-launcher PRIVATE /EHsc)
  endif()
  if(MINGW)
    target_link_options(mgspo-launcher PRIVATE -municode)
  endif()
  add_dependencies(mgspo-launcher mgspo-extract mgspo-lift)
  add_executable(mgspo-desktop WIN32 tools/launcher/desktop.cpp)
  target_compile_definitions(mgspo-desktop PRIVATE MGSPO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                                                  MGSPO_TOOL_REVISION="${MGSPO_TOOL_REVISION}")
  target_link_libraries(mgspo-desktop PRIVATE comdlg32 shell32)
  if(MSVC)
    target_compile_options(mgspo-desktop PRIVATE /EHsc)
  endif()
  if(MINGW)
    target_link_options(mgspo-desktop PRIVATE -municode)
  endif()
  add_dependencies(mgspo-desktop mgspo-launcher)
endif()

# Runtime that recompiled code links against.
add_library(mgspo_rt STATIC
  runtime/src/memory.cpp
  runtime/src/dispatch.cpp
  runtime/src/kernel.cpp
  runtime/src/sysmem.cpp
  runtime/src/threadman.cpp
  runtime/src/io.cpp
  runtime/src/display.cpp
  runtime/src/game_controls.cpp
  runtime/src/game_prompts.cpp
  runtime/src/ge.cpp
  runtime/src/gpu.cpp
  runtime/src/gpu_draw.cpp
  runtime/src/raster_fast.cpp
  runtime/src/audio.cpp
  runtime/src/sas_core.cpp
  runtime/src/sas.cpp
  runtime/src/atrac3.cpp
  runtime/src/atrac3plus.cpp
  runtime/src/atrac3p/bitstream.cpp
  runtime/src/atrac3p/dsp.cpp
  runtime/src/atrac3p/atrac_common.cpp
  runtime/src/atrac3p/compat.cpp
  runtime/src/atrac.cpp
  runtime/src/misc.cpp
  runtime/src/mpeg.cpp
  runtime/src/mpeg_demux.cpp
  runtime/src/font.cpp
  runtime/src/modules.cpp
  runtime/src/vfpu.cpp
)
if(WIN32)
  target_sources(mgspo_rt PRIVATE   runtime/src/platform_win32.cpp   runtime/src/video_decoder_mf.cpp runtime/src/sampler_win32.cpp)
      target_link_libraries(mgspo_rt PUBLIC user32 gdi32 winmm mfplat mfuuid ole32 dbghelp)
else()
  target_sources(mgspo_rt PRIVATE runtime/src/platform_null.cpp runtime/src/video_decoder_null.cpp runtime/src/sampler_null.cpp)
  find_package(Threads REQUIRED)
  target_link_libraries(mgspo_rt PUBLIC Threads::Threads)
endif()
target_include_directories(mgspo_rt PUBLIC runtime/include)
target_link_libraries(mgspo_rt PUBLIC mgspo)
if(MSVC AND MGSPO_PROFILE_SYMBOLS)
  target_compile_options(mgspo_rt PRIVATE /Zi)
endif()

add_executable(mgspo-at3dec tools/audio/at3dec.cpp)
target_link_libraries(mgspo-at3dec PRIVATE mgspo_rt)
target_include_directories(mgspo-at3dec PRIVATE runtime/src)

# Set -DMGSPO_GENERATED_DIR=<dir produced by mgspo-lift> to build the recompiled game.
set(MGSPO_GENERATED_DIR "" CACHE PATH "Output directory of mgspo-lift")
if(MGSPO_GENERATED_DIR)
  file(GLOB MGSPO_GENERATED_SRCS "${MGSPO_GENERATED_DIR}/*.cpp")
  add_executable(mgspo-game runtime/src/game_main.cpp ${MGSPO_GENERATED_SRCS})
  if(CMAKE_GENERATOR MATCHES "Ninja")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS mgspo_game_compile=1)
    set_property(TARGET mgspo-game PROPERTY JOB_POOL_COMPILE mgspo_game_compile)
  endif()
  target_include_directories(mgspo-game PRIVATE ${MGSPO_GENERATED_DIR})
  target_link_libraries(mgspo-game PRIVATE mgspo_rt mgspo)
  if(MSVC)
    target_compile_options(mgspo-game PRIVATE /W0 /bigobj)
    target_link_options(mgspo-game PRIVATE /STACK:16777216)
    if(MGSPO_PROFILE_SYMBOLS)
      target_compile_options(mgspo-game PRIVATE /Zi)
      target_link_options(mgspo-game PRIVATE /DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO)
    endif()
  endif()
endif()

option(MGSPO_BUILD_TESTS "Build unit tests" ON)
if(MGSPO_BUILD_TESTS)
  enable_testing()
  add_executable(mgspo-stage-tests tests/test_stage_inventory.cpp)
  add_test(NAME mgspo-stage-tests COMMAND mgspo-stage-tests)
  add_test(NAME mgspo-stage-cli COMMAND ${CMAKE_COMMAND}
           "-DLIFTER=$<TARGET_FILE:mgspo-lift>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/stage-cli-fixture"
           -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stage_cli.cmake")
  add_executable(mgspo-profile-tests tests/test_profile.cpp)
  target_include_directories(mgspo-profile-tests PRIVATE runtime/src runtime/include)
  add_test(NAME mgspo-profile-tests COMMAND mgspo-profile-tests)
  # Standalone raster checks avoid compiling the runtime or any generated game code.
  add_executable(mgspo-raster-tests tests/test_raster.cpp runtime/src/raster_fast.cpp)
  target_include_directories(mgspo-raster-tests PRIVATE runtime/src)
  if(NOT WIN32)
    target_link_libraries(mgspo-raster-tests PRIVATE Threads::Threads)
  endif()
  add_test(NAME mgspo-raster-tests COMMAND mgspo-raster-tests)
  foreach(case IN ITEMS valid high zero negative invalid overflow trailing)
    add_test(NAME mgspo-raster-threads-${case} COMMAND mgspo-raster-tests --threads)
  endforeach()
  set_tests_properties(mgspo-raster-threads-valid PROPERTIES ENVIRONMENT "MGSPO_GE_THREADS=2;MGSPO_TEST_THREADS=2")
  set_tests_properties(mgspo-raster-threads-high PROPERTIES ENVIRONMENT "MGSPO_GE_THREADS=99999;MGSPO_TEST_THREADS=6")
  set_tests_properties(mgspo-raster-threads-zero PROPERTIES ENVIRONMENT "MGSPO_GE_THREADS=0;MGSPO_TEST_THREADS=1")
  set_tests_properties(mgspo-raster-threads-negative PROPERTIES ENVIRONMENT "MGSPO_GE_THREADS=-8;MGSPO_TEST_THREADS=1")
  set_tests_properties(mgspo-raster-threads-invalid PROPERTIES ENVIRONMENT "MGSPO_GE_THREADS=bad;MGSPO_TEST_THREADS=1")
  set_tests_properties(mgspo-raster-threads-overflow PROPERTIES ENVIRONMENT "MGSPO_GE_THREADS=999999999999999999999;MGSPO_TEST_THREADS=1")
  set_tests_properties(mgspo-raster-threads-trailing PROPERTIES ENVIRONMENT "MGSPO_GE_THREADS=3bad;MGSPO_TEST_THREADS=1")
  if(WIN32)
    add_executable(mgspo-launcher-tests tests/test_launcher.cpp)
    target_link_libraries(mgspo-launcher-tests PRIVATE comdlg32)
    if(MSVC)
      target_compile_options(mgspo-launcher-tests PRIVATE /EHsc)
    endif()
    if(MINGW)
      target_link_options(mgspo-launcher-tests PRIVATE -municode)
    endif()
    add_test(NAME mgspo-launcher-tests COMMAND mgspo-launcher-tests)
    add_executable(mgspo-launcher-update-tests tests/test_launcher_update.cpp)
    target_link_libraries(mgspo-launcher-update-tests PRIVATE comdlg32)
    if(MSVC)
      target_compile_options(mgspo-launcher-update-tests PRIVATE /EHsc)
    endif()
    if(MINGW)
      target_link_options(mgspo-launcher-update-tests PRIVATE -municode)
    endif()
    add_test(NAME mgspo-launcher-update-tests COMMAND mgspo-launcher-update-tests)
    add_test(NAME mgspo-launcher-update-rebuild COMMAND mgspo-launcher-update-tests --rebuild)
    set_tests_properties(mgspo-launcher-update-tests mgspo-launcher-update-rebuild
                         PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
  endif()
  add_executable(mgspo-tests tests/test_main.cpp)
  target_link_libraries(mgspo-tests PRIVATE mgspo)
  add_test(NAME mgspo-tests COMMAND mgspo-tests)

  add_executable(mgspo-lifter-tests tests/test_lifter.cpp)
  target_link_libraries(mgspo-lifter-tests PRIVATE mgspo mgspo_rt)
  add_test(NAME mgspo-lifter-tests COMMAND mgspo-lifter-tests)

  add_executable(mgspo-runtime-tests tests/test_runtime.cpp)
  target_link_libraries(mgspo-runtime-tests PRIVATE mgspo_rt)
  add_test(NAME mgspo-runtime-tests COMMAND mgspo-runtime-tests)
  add_executable(mgspo-controls-tests tests/test_controls.cpp)
  target_link_libraries(mgspo-controls-tests PRIVATE mgspo_rt)
  add_test(NAME mgspo-controls-tests COMMAND mgspo-controls-tests)
  add_executable(mgspo-vfpu-tests tests/test_vfpu.cpp)
  target_link_libraries(mgspo-vfpu-tests PRIVATE mgspo mgspo_rt)
  add_test(NAME mgspo-vfpu-tests COMMAND mgspo-vfpu-tests)

  add_executable(mgspo-ge-tests tests/test_ge.cpp)
  target_link_libraries(mgspo-ge-tests PRIVATE mgspo_rt)
  target_include_directories(mgspo-ge-tests PRIVATE runtime/src)
  add_test(NAME mgspo-ge-tests COMMAND mgspo-ge-tests)

  add_executable(mgspo-audio-tests tests/test_audio.cpp)
  target_link_libraries(mgspo-audio-tests PRIVATE mgspo_rt)
  target_include_directories(mgspo-audio-tests PRIVATE runtime/src)
  add_test(NAME mgspo-audio-tests COMMAND mgspo-audio-tests)
  add_executable(mgspo-mpeg-tests tests/test_mpeg.cpp runtime/src/mpeg_demux.cpp)
  target_include_directories(mgspo-mpeg-tests PRIVATE runtime/src)
  add_test(NAME mgspo-mpeg-tests COMMAND mgspo-mpeg-tests)
endif()

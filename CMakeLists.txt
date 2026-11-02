cmake_minimum_required(VERSION 3.16)
project(mgspo_recomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_library(mgspo STATIC
  src/iso.cpp
  src/sfo.cpp
  src/bootfile.cpp
  src/elf.cpp
  src/mips.cpp
  src/prx.cpp
  src/functions.cpp
  src/emit.cpp
  src/vfpu.cpp
)
target_include_directories(mgspo PUBLIC src runtime/include)

add_executable(mgspo-extract tools/extract/main.cpp)
target_link_libraries(mgspo-extract PRIVATE mgspo)

add_executable(mgspo-lift tools/lift/main.cpp)
target_link_libraries(mgspo-lift PRIVATE mgspo)

# Runtime that recompiled code links against.
add_library(mgspo_rt STATIC
  runtime/src/memory.cpp
  runtime/src/dispatch.cpp
  runtime/src/kernel.cpp
  runtime/src/sysmem.cpp
  runtime/src/threadman.cpp
  runtime/src/io.cpp
  runtime/src/display.cpp
  runtime/src/ge.cpp
  runtime/src/gpu.cpp
  runtime/src/gpu_draw.cpp
  runtime/src/audio.cpp
  runtime/src/misc.cpp
  runtime/src/modules.cpp
  runtime/src/vfpu.cpp
)
if(WIN32)
  target_sources(mgspo_rt PRIVATE runtime/src/platform_win32.cpp)
  target_link_libraries(mgspo_rt PUBLIC user32 gdi32 winmm)
else()
  target_sources(mgspo_rt PRIVATE runtime/src/platform_null.cpp)
  find_package(Threads REQUIRED)
  target_link_libraries(mgspo_rt PUBLIC Threads::Threads)
endif()
target_include_directories(mgspo_rt PUBLIC runtime/include)
target_link_libraries(mgspo_rt PUBLIC mgspo)

# Set -DMGSPO_GENERATED_DIR=<dir produced by mgspo-lift> to build the recompiled game.
set(MGSPO_GENERATED_DIR "" CACHE PATH "Output directory of mgspo-lift")
if(MGSPO_GENERATED_DIR)
  file(GLOB MGSPO_GENERATED_SRCS "${MGSPO_GENERATED_DIR}/*.cpp")
  add_executable(mgspo-game runtime/src/game_main.cpp ${MGSPO_GENERATED_SRCS})
  target_include_directories(mgspo-game PRIVATE ${MGSPO_GENERATED_DIR})
  target_link_libraries(mgspo-game PRIVATE mgspo_rt mgspo)
  if(MSVC)
    target_compile_options(mgspo-game PRIVATE /W0 /bigobj)
    target_link_options(mgspo-game PRIVATE /STACK:16777216)
  endif()
endif()

option(MGSPO_BUILD_TESTS "Build unit tests" ON)
if(MGSPO_BUILD_TESTS)
  enable_testing()
  add_executable(mgspo-tests tests/test_main.cpp)
  target_link_libraries(mgspo-tests PRIVATE mgspo)
  add_test(NAME mgspo-tests COMMAND mgspo-tests)

  add_executable(mgspo-lifter-tests tests/test_lifter.cpp)
  target_link_libraries(mgspo-lifter-tests PRIVATE mgspo mgspo_rt)
  add_test(NAME mgspo-lifter-tests COMMAND mgspo-lifter-tests)

  add_executable(mgspo-runtime-tests tests/test_runtime.cpp)
  target_link_libraries(mgspo-runtime-tests PRIVATE mgspo_rt)
  add_test(NAME mgspo-runtime-tests COMMAND mgspo-runtime-tests)
  add_executable(mgspo-vfpu-tests tests/test_vfpu.cpp)
  target_link_libraries(mgspo-vfpu-tests PRIVATE mgspo mgspo_rt)
  add_test(NAME mgspo-vfpu-tests COMMAND mgspo-vfpu-tests)

  add_executable(mgspo-ge-tests tests/test_ge.cpp)
  target_link_libraries(mgspo-ge-tests PRIVATE mgspo_rt)
  target_include_directories(mgspo-ge-tests PRIVATE runtime/src)
  add_test(NAME mgspo-ge-tests COMMAND mgspo-ge-tests)
endif()

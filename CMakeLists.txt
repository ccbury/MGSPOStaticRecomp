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
)
target_include_directories(mgspo PUBLIC src)

add_executable(mgspo-extract tools/extract/main.cpp)
target_link_libraries(mgspo-extract PRIVATE mgspo)

option(MGSPO_BUILD_TESTS "Build unit tests" ON)
if(MGSPO_BUILD_TESTS)
  enable_testing()
  add_executable(mgspo-tests tests/test_main.cpp)
  target_link_libraries(mgspo-tests PRIVATE mgspo)
  add_test(NAME mgspo-tests COMMAND mgspo-tests)
endif()

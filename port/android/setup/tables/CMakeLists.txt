# The runtime tables (tables.h): a static library, its command line and tests. The library needs patcher.h only; the command
# line and the tests link setup/patcher and setup/container, built here from their folders. Nothing else in the project
# builds or links this yet.
#   PC:     cmake -G Ninja -S port/android/setup/tables -B <dir> && ninja -C <dir>
#   Device: build_android.sh <dir>
#   -DTOKT_SANITIZE=ON: ASan and UBSan, all three libraries. -DTOKT_FUZZER=ON: also the libFuzzer target (clang).
cmake_minimum_required(VERSION 3.22)
project(tok_tables C)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
option(TOKT_SANITIZE "build with ASan and UBSan" OFF)
option(TOKT_FUZZER "build the libFuzzer target" OFF)
if(TOKT_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()
if(TOKT_FUZZER)
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

# Both folders name a test target fuzz_malformed, and they skip their tests on Android: seen as Android here, they build
# their libraries and command lines only.
set(TOKT_ON_ANDROID ${ANDROID})
set(ANDROID TRUE)
add_subdirectory(../patcher patcher)
add_subdirectory(../container container)
set(ANDROID ${TOKT_ON_ANDROID})

add_library(toktables STATIC tables.c sha256.c sha256_hw.c)
target_include_directories(toktables PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../patcher)
target_compile_options(toktables PRIVATE -Wall -Wextra -Wshadow -Wconversion)
set_target_properties(toktables PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(tables_cli tables_cli.c rewritten_files.c)
target_compile_options(tables_cli PRIVATE -Wall -Wextra)
target_link_libraries(tables_cli PRIVATE toktables tokpatcher tokcontainer)

# Tests (regress.sh runs them).
if(NOT ANDROID)
  add_executable(fuzz_malformed tests/fuzz_malformed.c tests/fuzz_common.c rewritten_files.c)
  target_link_libraries(fuzz_malformed PRIVATE toktables tokpatcher)
  add_executable(test_units tests/test_units.c)
  target_include_directories(test_units PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../patcher)
  target_link_libraries(test_units PRIVATE toktables)
  if(TOKT_FUZZER)
    add_executable(fuzz_libfuzzer tests/fuzz_libfuzzer.c tests/fuzz_common.c rewritten_files.c)
    target_link_libraries(fuzz_libfuzzer PRIVATE toktables tokpatcher)
    target_link_options(fuzz_libfuzzer PRIVATE -fsanitize=fuzzer)
  endif()
endif()

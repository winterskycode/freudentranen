# The game-code rewriter in C (patcher.h): a static library, its command line and tests. Stands alone; nothing else in the
# project builds or links it yet. Capstone comes from fetch_capstone.sh (build_deps.sh runs it).
#   PC:     cmake -G Ninja -S port/android/setup/patcher -B <dir> && ninja -C <dir>
#   Device: add -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33
#   -DTOKP_SANITIZE=ON: ASan and UBSan, the rewriter and Capstone both.
cmake_minimum_required(VERSION 3.22)
project(tok_patcher C)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CAPSTONE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/src/capstone CACHE PATH "Capstone 5.0.9 sources")
option(TOKP_SANITIZE "build with ASan and UBSan" OFF)
if(NOT EXISTS ${CAPSTONE_SRC}/cs.c)
  message(FATAL_ERROR "no Capstone in ${CAPSTONE_SRC}: run port/android/setup/patcher/fetch_capstone.sh")
endif()
if(TOKP_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# The AArch64 decoder only, with Capstone's own default options for it.
add_library(tok_capstone STATIC
  ${CAPSTONE_SRC}/cs.c ${CAPSTONE_SRC}/Mapping.c ${CAPSTONE_SRC}/MCInst.c ${CAPSTONE_SRC}/MCInstrDesc.c
  ${CAPSTONE_SRC}/MCRegisterInfo.c ${CAPSTONE_SRC}/SStream.c ${CAPSTONE_SRC}/utils.c
  ${CAPSTONE_SRC}/arch/AArch64/AArch64BaseInfo.c ${CAPSTONE_SRC}/arch/AArch64/AArch64Disassembler.c
  ${CAPSTONE_SRC}/arch/AArch64/AArch64InstPrinter.c ${CAPSTONE_SRC}/arch/AArch64/AArch64Mapping.c
  ${CAPSTONE_SRC}/arch/AArch64/AArch64Module.c)
target_compile_definitions(tok_capstone PRIVATE CAPSTONE_HAS_ARM64 CAPSTONE_USE_SYS_DYN_MEM)
target_include_directories(tok_capstone PRIVATE ${CAPSTONE_SRC}/include ${CAPSTONE_SRC})
target_compile_options(tok_capstone PRIVATE -w)
set_target_properties(tok_capstone PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(tokpatcher STATIC nso.c patcher.c)
target_include_directories(tokpatcher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${CAPSTONE_SRC}/include)
target_compile_options(tokpatcher PRIVATE -Wall -Wextra -Wshadow -Wconversion)
set_target_properties(tokpatcher PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(tokpatcher PUBLIC tok_capstone)

add_executable(patcher_cli patcher_cli.c)
target_compile_options(patcher_cli PRIVATE -Wall -Wextra)
target_link_libraries(patcher_cli PRIVATE tokpatcher)

# Tests (regress.sh runs them)
if(NOT ANDROID)
  add_executable(fuzz_malformed tests/fuzz_malformed.c)
  target_link_libraries(fuzz_malformed PRIVATE tokpatcher)
  add_executable(decode_dump tests/decode_dump.c)
  target_include_directories(decode_dump PRIVATE ${CAPSTONE_SRC}/include)
  target_link_libraries(decode_dump PRIVATE tok_capstone)
endif()

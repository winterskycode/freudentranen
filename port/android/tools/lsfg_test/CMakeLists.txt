# lsfg_test: the vendored LSFG library on two pictures, outside the game (PC or device).
#   cmake -S port/android/tools/lsfg_test -B work/lsfg/test-pc -G Ninja && ninja -C work/lsfg/test-pc
#   device: add -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35
cmake_minimum_required(VERSION 3.16)
project(lsfg_test LANGUAGES C CXX)
set(VK ${CMAKE_CURRENT_SOURCE_DIR}/../../vk)
set(LSFG ${VK}/third_party/lsfg_framegen)
file(GLOB LSFG_SOURCES ${LSFG}/src/*/*.cpp ${LSFG}/v3.1_src/*.cpp ${LSFG}/v3.1_src/*/*.cpp ${LSFG}/v3.1p_src/*.cpp ${LSFG}/v3.1p_src/*/*.cpp)
add_executable(lsfg_test main.cpp ${LSFG_SOURCES} ${VK}/lsfg_bridge.cpp ${VK}/third_party/volk/volk.c)
target_include_directories(lsfg_test PRIVATE ${LSFG}/include ${LSFG}/public ${LSFG}/v3.1_include ${LSFG}/v3.1p_include ${VK}/third_party/volk ${VK})
target_compile_definitions(lsfg_test PRIVATE VK_NO_PROTOTYPES)
set_target_properties(lsfg_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(lsfg_test PRIVATE -O2 -Wno-everything)
target_link_libraries(lsfg_test PRIVATE ${CMAKE_DL_LIBS})

# PC tool: the app's shader translation from a Lossless.dll, for development. All its sources are in this repository.
#   cmake -S port/android/tools/lsfg_extract -B work/lsfg/build && cmake --build work/lsfg/build
#   work/lsfg/build/lsfg_extract <Lossless.dll> work/lsfg/shaders
cmake_minimum_required(VERSION 3.16)
project(lsfg_extract LANGUAGES CXX)
include(../../host/lsfg_translate.cmake)
add_executable(lsfg_extract main.cpp ${LSFG_TRANSLATE_SOURCES})
set_target_properties(lsfg_extract PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(lsfg_extract SYSTEM PRIVATE ${LSFG_TRANSLATE_INCLUDES})

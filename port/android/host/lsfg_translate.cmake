# The sources of the shader translator (host/lsfg_translate.cpp) for port/android/CMakeLists.txt and tools/lsfg_extract.
set(LSFG_HOST ${CMAKE_CURRENT_LIST_DIR})
set(LSFG_THIRD_PARTY ${LSFG_HOST}/../vk/third_party)
file(GLOB LSFG_TRANSLATE_SOURCES ${LSFG_THIRD_PARTY}/dxbc/src/*/*.cpp ${LSFG_THIRD_PARTY}/pe-parse/src/*.cpp)
list(APPEND LSFG_TRANSLATE_SOURCES ${LSFG_HOST}/lsfg_translate.cpp)
set(LSFG_TRANSLATE_INCLUDES ${LSFG_HOST} ${LSFG_THIRD_PARTY}/pe-parse/include ${LSFG_THIRD_PARTY}/dxbc/include/dxbc
    ${LSFG_THIRD_PARTY}/dxbc/include/spirv ${LSFG_THIRD_PARTY}/dxbc/include/util ${LSFG_THIRD_PARTY}/dxbc/include/dxvk)

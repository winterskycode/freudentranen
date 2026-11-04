DXVK's DXBC compiler, vendored from https://github.com/PancakeTAS/dxbc at commit 78ab59a (the commit lsfg-vk v1.0.0 pins):
DXVK's shader translation, cut out of DXVK (https://github.com/doitsujin/dxvk) to run on its own. zlib licence, see
LICENSE.md; include/spirv/thirdparty holds the Khronos SPIR-V headers under their own licence (in the files themselves).
It holds no shaders. Copied without its CMakeLists.txt (host/lsfg_translate.cmake lists the sources).
Used only by port/android/host/lsfg_translate.cpp (libtotklsfg.so and tools/lsfg_extract). No local changes.

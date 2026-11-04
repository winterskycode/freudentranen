pe-parse 2.1.1, vendored from https://github.com/trailofbits/pe-parse at commit 31ac596 (the commit lsfg-vk v1.0.0 pins;
MIT, see LICENSE). Only the library: include/ and src/ are pe-parser-library/include and src without the Windows-only
unicode_winapi.cpp; no tests, tools or build files (host/lsfg_translate.cmake lists the sources).
Used only by port/android/host/lsfg_translate.cpp (libtotklsfg.so and tools/lsfg_extract). No local changes.

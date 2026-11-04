// lsfg_extract: the startup screen's shader translation (host/lsfg_translate.cpp) on the PC, for development and for
// checking it against earlier output. What it writes is the user's licensed data: never into the repository.
//
// usage: lsfg_extract <Lossless.dll> <output directory>   ->   <output>/<name>.spv, one per shader
#include "lsfg_translate.h"

#include <cstdio>
#include <filesystem>

int main(int argc, char **argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: %s <Lossless.dll> <output directory>\n", argv[0]); return 2; }
    std::error_code failed;
    std::filesystem::create_directories(argv[2], failed);
    if (failed) { std::fprintf(stderr, "cannot make %s\n", argv[2]); return 1; }
    std::string error = lsfg_translate(argv[1], argv[2], nullptr, nullptr);
    if (!error.empty()) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    std::printf("%d shaders written to %s\n", LSFG_SHADER_COUNT, argv[2]);
    return 0;
}

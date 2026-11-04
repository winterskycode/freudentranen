// Lossless Scaling's frame-generation shaders as SPIR-V, from the player's own Lossless.dll. Used by the startup screen
// (libtotklsfg.so, Lsfg.java) and by tools/lsfg_extract on the PC. Nothing here runs in the game's process.
#pragma once
#include <string>

#define LSFG_SHADER_COUNT 52   // the shaders of the table in lsfg_translate.cpp (Lsfg.java and vk/overlay.cpp count them)

using lsfg_progress = void (*)(void *user, int done, int total);

// Writes <out_dir>/<name>.spv for every shader; out_dir must exist. Returns "", or what went wrong in words for the player
// (a message, never an exception: the DLL is not ours and may be anything). A failure can leave some files in out_dir.
std::string lsfg_translate(const char *dll, const char *out_dir, lsfg_progress progress, void *user) noexcept;

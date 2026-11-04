/* The 1.0.0 side of the mod merge (Mods.java): what 1.0.0 had where 1.4.3 differs, so that a mod made for an older version can
   be merged three ways. The bytes of mod-base.bin, made on the device from the player's two files instead of on a PC
   (tools/make_mod_base.py, which also gives the format). They hold game content, so they live in private storage only.

   A file the update's table maps to its own place in the base game is the same file and is not read, so this reads only what
   the update changed. Everything read is checked: a damaged file gives an error, never a read or write outside the buffers.
   Nothing here prints or writes files. */
#ifndef TOK_MODBASE_H
#define TOK_MODBASE_H
#include "container.h"

/* Called now and then from any of the workers, one at a time. */
typedef void (*tokm_progress)(void *ctx, uint64_t done, uint64_t total);

/* *bytes is malloc'd and the caller's to free. threads 0: as many as the machine has, at most 4 (each holds a pack and its
   compressor). The result does not depend on the threads. */
int tokm_make(const tokc_game *g, int threads, tokm_progress progress, void *ctx, uint8_t **bytes, size_t *size, char *err, size_t errlen);

/* For the tests. */
typedef struct { uint64_t files, read_files, read_bytes, rows, packs; } tokm_stats;
void tokm_last_stats(tokm_stats *out);
int tokm_pack_blob(const uint8_t *mine, uint64_t mine_size, const uint8_t *theirs, uint64_t theirs_size, uint8_t **blob, size_t *blob_size, char *err, size_t errlen);
#endif

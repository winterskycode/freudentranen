#ifndef TOK_CONTAINER_FUZZ_COMMON_H
#define TOK_CONTAINER_FUZZ_COMMON_H
#include "synth.h"

/* Damage to one plain structure before it is hashed and encrypted, a word at an offset. */
typedef struct { uint8_t region, width; uint32_t at; uint64_t value; } fz_edit;
typedef struct { const fz_edit *edits; size_t count; } fz_plan;
void fz_hook(void *plan, sy_region r, uint8_t *data, size_t size);

/* Every call of the reader on these bytes, with buffers of exact size. Returns 1 if the pair was accepted. */
int fz_exercise(const uint8_t *base, size_t base_size, const uint8_t *update, size_t update_size, const char *keys, size_t keys_size, uint64_t seed);
#endif

/* Shared by the two fuzzers. */
#include "fuzz_common.h"
#include "../container_internal.h"
#include <stdlib.h>
#include <unistd.h>

void fz_hook(void *user, sy_region r, uint8_t *data, size_t size) {
    const fz_plan *p = user;
    for (size_t i = 0; i < p->count; i++) {
        const fz_edit *e = &p->edits[i];
        if (e->region != r || !size) continue;
        size_t at = e->at % size;
        for (unsigned b = 0; b < e->width && at + b < size; b++) data[at + b] = (uint8_t)(e->value >> 8 * b);
    }
}

int fz_exercise(const uint8_t *base, size_t base_size, const uint8_t *update, size_t update_size, const char *keys, size_t keys_size, uint64_t seed) {
    tokc_keys *k = NULL;
    char err[120];
    /* an exact copy: a read past the text is a read past the allocation */
    char *text = malloc(keys_size ? keys_size : 1);
    if (keys_size) memcpy(text, keys, keys_size);
    int rc = tokc_keys_parse(text, keys_size, &k, err, sizeof err);
    free(text);
    if (rc) return 0;
    int bf = sy_fd(base, base_size), uf = sy_fd(update, update_size), accepted = 0;
    tokc_info info;
    tokc_inspect(bf, k, &info, err, sizeof err);
    tokc_inspect(uf, k, &info, err, sizeof err);
    tokc_game *g = NULL;
    rc = tokc_game_open(bf, uf, k, &g, err, sizeof err);
    tokc_keys_free(k);
    if (!rc) {
        accepted = 1;
        for (int m = 0; m < TOKC_MODULE_COUNT; m++) {
            uint8_t *b;
            size_t n;
            if (!tokc_game_module(g, (tokc_module)m, &b, &n, err, sizeof err)) free(b);
        }
        uint8_t *idx;
        size_t idx_size;
        if (!tokc_romfs_index(g, &idx, &idx_size, err, sizeof err)) free(idx);
        uint64_t size = tokc_romfs_size(g), s = seed * 2 + 1;
        for (int i = 0; i < 24; i++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            uint64_t off = i < 4 ? size - (size > (uint64_t)i ? (uint64_t)i : 0) : s % (size + 16);
            size_t n = (size_t)(s >> 40) % (1u << (i % 18));
            uint8_t *b = malloc(n ? n : 1);
            /* writes must stop at what it says it read */
            int64_t got = tokc_romfs_pread(g, b, n, off);
            if (got > (int64_t)n) abort();
            free(b);
        }
        uint64_t runs = tokc_romfs_runs(g, size / 2, size);
        tokc_stats st;
        tokc_game_stats(g, &st);
        (void)runs;
        tokc_game_close(g);
    }
    close(bf); close(uf);
    return accepted;
}

/* libFuzzer: the input is a list of edits to a made-up set of modules, so that coverage guides the damage. Twelve bytes an edit:
   module and region, width, offset (32 bits), value (48 bits). TOKT_FUZZ_CASE names the case folder (gen_synth.py). */
#include "fuzz_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EDITS 32
static fz_case c;

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    const char *dir = getenv("TOKT_FUZZ_CASE");
    char rewritten[4096], exefs[4096];
    if (!dir) { fprintf(stderr, "TOKT_FUZZ_CASE is not set\n"); abort(); }
    snprintf(rewritten, sizeof rewritten, "%s/rewritten/v143", dir);
    snprintf(exefs, sizeof exefs, "%s/extracted/v143/exefs", dir);
    fz_load(&c, rewritten, exefs);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    for (size_t i = 0; i < MAX_EDITS && 12 * (i + 1) <= size; i++) {
        const uint8_t *e = data + 12 * i;
        uint64_t off = (uint64_t)e[2] | (uint64_t)e[3] << 8 | (uint64_t)e[4] << 16 | (uint64_t)e[5] << 24, value = 0;
        for (int k = 0; k < 6; k++) value |= (uint64_t)e[6 + k] << 8 * k;
        /* so that negative words are reachable */
        if (e[1] & 0x80) value |= 0xFFFF000000000000ull;
        fz_edit(&c, e[0] % TOKT_MODULE_COUNT, (e[0] >> 2) % FZ_REGIONS, off, 1u << (e[1] & 3), value);
    }
    fz_run(&c, NULL);
    fz_revert(&c);
    return 0;
}

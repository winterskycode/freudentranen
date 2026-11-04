/* libFuzzer: the input is a list of edits to the plain structures of a made-up pair, so that coverage guides the damage past
   the hashes and the encryption. Twelve bytes an edit: region, width, offset (32 bits), value (48 bits). The first byte, when
   it has its top bit set, sends the rest to the key parser and to a raw base file instead. */
#include "fuzz_common.h"
#include "../container_internal.h"
#include <stdlib.h>

#define MAX_EDITS 32

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!size) return 0;
    fz_edit edits[MAX_EDITS];
    fz_plan plan = {edits, 0};
    uint8_t mode = data[0];
    data++; size--;
    sy_spec spec;
    sy_default(&spec, 1 + (mode & 7));
    spec.xci = mode >> 3 & 1;
    spec.pieces = 1 + (mode >> 4 & 3) * 300u;
    spec.subsections = 1 + (mode >> 4 & 3) * 400u;
    sy_game g;
    if (mode & 0x80) {
        spec.pieces = 8; spec.subsections = 4;
        sy_make(&g, &spec);
        if (mode & 0x40) fz_exercise(g.base.data, g.base.size, g.update.data, g.update.size, (const char *)data, size, mode);
        else fz_exercise(data, size, g.update.data, g.update.size, g.keys_text, g.keys_size, mode);
        sy_free(&g);
        return 0;
    }
    for (; size >= 12 && plan.count < MAX_EDITS; data += 12, size -= 12) {
        uint64_t value = 0;
        for (int i = 0; i < 6; i++) value |= (uint64_t)data[6 + i] << 8 * i;
        edits[plan.count++] = (fz_edit){(uint8_t)(data[0] % SY_REGION_COUNT), (uint8_t)(1 + data[1] % 8), rd32(data + 2), value};
    }
    spec.hook = fz_hook; spec.user = &plan;
    sy_make(&g, &spec);
    fz_exercise(g.base.data, g.base.size, g.update.data, g.update.size, g.keys_text, g.keys_size, 1);
    sy_free(&g);
    return 0;
}

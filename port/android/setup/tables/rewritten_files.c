/* rewritten_files.h */
#include "rewritten_files.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int tokt_read_file(const char *path, uint8_t **data, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    rewind(f);
    uint8_t *p = n < 0 ? NULL : malloc(n ? (size_t)n : 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return -1; }
    fclose(f);
    *data = p; *size = (size_t)n;
    return 0;
}

static int fixups(tokp_layout *l, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256], key[64];
    uint64_t value;
    unsigned found = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "# %63s %" SCNx64, key, &value) != 2) continue;
        if (!strcmp(key, "PATCH_OFF")) { l->patch_off = value; found |= 1; }
        else if (!strcmp(key, "KEY_PAGE_OFF")) { l->key_page_off = value; found |= 2; }
        else if (!strcmp(key, "TEXT_END")) { l->text_end = value; found |= 4; }
        else if (!strcmp(key, "RO_OFF")) { l->ro_off = value; found |= 8; }
        else if (!strcmp(key, "DATA_OFF")) { l->data_off = value; found |= 16; }
    }
    fclose(f);
    return found == 31 ? 0 : -1;
}

static int gates(tokp_result *r, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256], kind[32];
    size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        uint64_t ret, original;
        unsigned arg;
        if (sscanf(line, "%" SCNx64 " %31s %u %" SCNx64, &ret, kind, &arg, &original) != 4) { fclose(f); return -1; }
        uint32_t k = 0;
        while (k < TOKP_KIND_COUNT && strcmp(kind, tokp_kind_names[k])) k++;
        if (k == TOKP_KIND_COUNT) { fclose(f); return -1; }
        if (r->ngates == cap) {
            cap = cap ? 2 * cap : 64;
            tokp_gate *g = realloc(r->gates, cap * sizeof *g);
            if (!g) { fclose(f); return -1; }
            r->gates = g;
        }
        r->gates[r->ngates++] = (tokp_gate){ret, original, k, arg};
    }
    fclose(f);
    return 0;
}

int tokt_files_load(tokt_files *f, const char *rewritten, const char *exefs, const char *name, char *err, size_t errlen) {
    char path[4096];
    size_t size = 0;
    memset(f, 0, sizeof *f);
    snprintf(path, sizeof path, "%s/%s", exefs, name);
    if (tokt_read_file(path, &f->nso, &f->nso_size)) goto bad;
    snprintf(path, sizeof path, "%s/%s.patched.bin", rewritten, name);
    if (tokt_read_file(path, &f->image, &size)) goto bad;
    f->rewrite.image_size = size;
    snprintf(path, sizeof path, "%s/%s.fixups.tsv", rewritten, name);
    if (fixups(&f->rewrite.layout, path)) goto bad;
    f->rewrite.patch_size = size - f->rewrite.layout.patch_off;
    snprintf(path, sizeof path, "%s/%s.gates.tsv", rewritten, name);
    if (gates(&f->rewrite, path)) goto bad;
    return 0;
bad:
    snprintf(err, errlen, "cannot read %s", path);
    tokt_files_free(f);
    return -1;
}

void tokt_files_free(tokt_files *f) {
    free(f->nso); free(f->image); free(f->rewrite.gates);
    memset(f, 0, sizeof *f);
}

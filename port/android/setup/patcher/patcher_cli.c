/* patcher_cli: the rewriter on files, with the Python tool's arguments and output files.
     patcher_cli [--time] <NSO file> <output dir> <module name>
   Writes <name>.patched.bin, the side files the Python tool writes (fixups, gates, sites, cuts, params, obs .tsv, .target)
   and build_src.sha256. --time prints the rewrite's time in memory, without the file reading and writing. */
#include "patcher.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef unsigned long long ull;

static int read_file(const char *path, uint8_t **data, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); return -1; }
    if (fseek(f, 0, SEEK_END) || ftell(f) < 0) { fprintf(stderr, "%s: cannot seek\n", path); fclose(f); return -1; }
    size_t n = (size_t)ftell(f);
    rewind(f);
    uint8_t *p = malloc(n ? n : 1);
    if (!p || fread(p, 1, n, f) != n) { fprintf(stderr, "%s: cannot read\n", path); free(p); fclose(f); return -1; }
    fclose(f);
    *data = p; *size = n;
    return 0;
}

static FILE *open_out(const char *dir, const char *name, const char *ext) {
    char path[4096];
    if (snprintf(path, sizeof path, "%s/%s%s%s", dir, name, ext[0] ? "." : "", ext) >= (int)sizeof path) { fprintf(stderr, "path too long\n"); return NULL; }
    FILE *f = fopen(path, "wb");
    if (!f) fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return f;
}

static int make_dirs(const char *dir) {
    char p[4096];
    if (snprintf(p, sizeof p, "%s", dir) >= (int)sizeof p) return -1;
    for (char *c = p + 1; *c; c++) if (*c == '/') { *c = 0; if (mkdir(p, 0777) && errno != EEXIST) return -1; *c = '/'; }
    return mkdir(p, 0777) && errno != EEXIST ? -1 : 0;
}

static int write_tsvs(const char *dir, const char *name, const tokp_result *r) {
    const tokp_layout *l = &r->layout;
    static const char *const keys[8] = {"K18", "KTLS", "KTLR", "KGATE", "KRES0", "KRES1", "KASPECT", "KASPECTUI"};
    FILE *f;
    if (!(f = open_out(dir, name, "target"))) return -1;
    fprintf(f, "%s\n", TOKP_TARGET); fclose(f);
    if (!(f = open_out(dir, "build_src.sha256", ""))) return -1;
    fprintf(f, "%s  patch_nso.py\n", TOKP_PY_SHA256); fclose(f);
    if (!(f = open_out(dir, name, "fixups.tsv"))) return -1;
    fprintf(f, "# PATCH_OFF\t0x%llx\n# KEY_PAGE_OFF\t0x%llx\n# KEY_PAGE_SIZE\t0x%x\n# TEXT_END\t0x%llx\n# RO_OFF\t0x%llx\n# DATA_OFF\t0x%llx\n",
            (ull)l->patch_off, (ull)l->key_page_off, TOKP_KEY_PAGE_SIZE, (ull)l->text_end, (ull)l->ro_off, (ull)l->data_off);
    for (int i = 0; i < 8; i++) fprintf(f, "# KEY_%s_IDX\t%d\n", keys[i], i);
    fclose(f);
    if (!(f = open_out(dir, name, "gates.tsv"))) return -1;
    for (size_t i = 0; i < r->ngates; i++) {
        const tokp_gate *g = &r->gates[i];
        fprintf(f, "0x%llx\t%s\t%u\t0x%llx\n", (ull)g->ret, tokp_kind_names[g->kind], g->arg, (ull)g->original);
    }
    fclose(f);
    if (!(f = open_out(dir, name, "sites.tsv"))) return -1;
    for (size_t i = 0; i < r->nsites; i++) fprintf(f, "0x%llx\t%s\n", (ull)r->sites[i].off, tokp_kind_names[r->sites[i].kind]);
    fclose(f);
    static const char *const empty[3] = {"cuts.tsv", "params.tsv", "obs.tsv"};
    for (int i = 0; i < 3; i++) {
        if (!(f = open_out(dir, name, empty[i]))) return -1;
        fprintf(f, "# BUILD_ID\t%s\n", l->build_id); fclose(f);
    }
    return 0;
}

/* The Python tool's last line: site counts by first appearance. */
static void print_summary(const char *name, const tokp_result *r) {
    unsigned count[TOKP_KIND_COUNT] = {0}, seen = 0;
    int order[TOKP_KIND_COUNT];
    for (size_t i = 0; i < r->nsites; i++) if (!count[r->sites[i].kind]++) order[seen++] = (int)r->sites[i].kind;
    printf("%s {", name);
    for (unsigned i = 0; i < seen; i++) printf("%s'%s': %u", i ? ", " : "", tokp_kind_names[order[i]], count[order[i]]);
    printf("} cuts 0 obs 0 patch %llu PATCH_OFF 0x%llx slot_refs %u gates %zu\n", (ull)r->patch_size, (ull)r->layout.patch_off, r->slot_refs, r->ngates);
}

int main(int argc, char **argv) {
    int timing = argc > 1 && !strcmp(argv[1], "--time");
    if (argc != 4 + timing) { fprintf(stderr, "usage: %s [--time] <NSO file> <output dir> <module name>\n", argv[0]); return 2; }
    const char *src = argv[1 + timing], *dir = argv[2 + timing], *name = argv[3 + timing];
    uint8_t *nso, *image;
    size_t size;
    if (read_file(src, &nso, &size)) return 1;
    char err[256] = "";
    tokp_result r;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = tokp_rewrite_alloc(nso, size, &image, &r, err, sizeof err);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(nso);
    if (rc) { fprintf(stderr, "%s: %s (%d)\n", src, err, rc); return 1; }
    if (timing) fprintf(stderr, "%s: %.1f ms\n", name, (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6);
    if (make_dirs(dir)) { fprintf(stderr, "%s: %s\n", dir, strerror(errno)); return 1; }
    FILE *f = open_out(dir, name, "patched.bin");
    if (!f) return 1;
    if (fwrite(image, 1, r.image_size, f) != r.image_size || fclose(f)) { fprintf(stderr, "cannot write the image\n"); return 1; }
    if (write_tsvs(dir, name, &r)) return 1;
    print_summary(name, &r);
    free(image);
    tokp_result_free(&r);
    return 0;
}

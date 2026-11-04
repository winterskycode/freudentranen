/* mod-base.bin on a PC, for the comparison with tools/make_mod_base.py:
     modbase_cli make <prod.keys> <base> <update> <out> [--threads N]
     modbase_cli index <prod.keys> <base> <update> <out>      the base game's runtime-index.bin */
#include "modbase.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int open_in(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror(path); exit(2); }
    return fd;
}
static void write_out(const char *path, const uint8_t *b, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(b, 1, n, f) != n || fclose(f)) { perror(path); exit(2); }
}
static void progress(void *ctx, uint64_t done, uint64_t total) { (void)ctx; fprintf(stderr, "\r%llu / %llu ", (unsigned long long)done, (unsigned long long)total); }
int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: modbase_cli make|index <prod.keys> <base> <update> <out> [--threads N]\n"); return 2; }
    int threads = argc == 8 && !strcmp(argv[6], "--threads") ? atoi(argv[7]) : 0;
    char err[300]; tokc_keys *keys = NULL; tokc_game *g = NULL; uint8_t *bytes = NULL; size_t size = 0;
    if (tokc_keys_parse_fd(open_in(argv[2]), &keys, err, sizeof err) || tokc_game_open(open_in(argv[3]), open_in(argv[4]), keys, &g, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    tokc_keys_free(keys);
    double t = now();
    if (!strcmp(argv[1], "index")) {
        if (tokc_base_romfs_index(g, &bytes, &size, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    } else {
        if (tokm_make(g, threads, progress, NULL, &bytes, &size, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
        tokm_stats s; tokm_last_stats(&s);
        fprintf(stderr, "\n%llu files in both, %llu read (%.1f MB), %llu differ, %llu packs\n", (unsigned long long)s.files, (unsigned long long)s.read_files, s.read_bytes / 1e6, (unsigned long long)s.rows, (unsigned long long)s.packs);
    }
    printf("%zu bytes in %.1f s\n", size, now() - t);
    write_out(argv[5], bytes, size);
    free(bytes); tokc_game_close(g);
    return 0;
}

/* container_cli: the reader on real files, for checking and timing on the PC and on a device.
     container_cli selftest
     container_cli info <prod.keys> <file>...
     container_cli check <prod.keys> <base> <update> [--threads N] [--seed N]
         [--modules DIR] [--index runtime-index.bin] [--files runtime-index.bin IMAGE] [--image IMAGE] [--random N IMAGE]
     container_cli bench <prod.keys> <base> <update> [--threads N] [--seed N] [--image IMAGE] [--cold] [--samples N] [--seq-gb N]
   check compares in memory: no decrypted bytes reach a disk. Everything printed is counts, sizes, offsets and times, never
   bytes of the files or a key. IMAGE is the merged romfs.bin. */
#define _GNU_SOURCE 1
#include "container_internal.h"
#include "tests/selftest.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef unsigned long long ull;
#define BLOCK (4u << 20)

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static uint64_t rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

static int open_in(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); exit(2); }
    return fd;
}

static uint8_t *read_all(const char *path, size_t *size) {
    int fd = open_in(path);
    struct stat st;
    if (fstat(fd, &st)) { perror(path); exit(2); }
    uint8_t *p = malloc((size_t)st.st_size + 1);
    if (!p || tokc_pread_full(fd, p, (size_t)st.st_size, 0)) { fprintf(stderr, "%s: cannot read\n", path); exit(2); }
    close(fd);
    *size = (size_t)st.st_size;
    return p;
}

static tokc_keys *load_keys(const char *path) {
    char err[256];
    tokc_keys *k;
    int fd = open_in(path), rc = tokc_keys_parse_fd(fd, &k, err, sizeof err);
    close(fd);
    if (rc) { fprintf(stderr, "keys: %s\n", err); exit(2); }
    return k;
}

static const char *type_name(uint32_t t) { return t == TOKC_META_APPLICATION ? "application" : t == TOKC_META_PATCH ? "patch" : t == TOKC_META_ADDON ? "add-on" : "other"; }

static int cmd_info(int argc, char **argv) {
    if (argc < 2) return 2;
    tokc_keys *k = load_keys(argv[0]);
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        char err[256];
        tokc_info info;
        int fd = open_in(argv[i]), rc = tokc_inspect(fd, k, &info, err, sizeof err);
        close(fd);
        if (rc) { printf("%s: error %d: %s\n", argv[i], rc, err); bad = 1; continue; }
        printf("%s: %s, title %016llx, %s, version %u, %d content meta\n", argv[i], info.is_xci ? "XCI" : "NSP", (ull)info.title_id, type_name(info.type), info.version, info.metas);
    }
    tokc_keys_free(k);
    return bad;
}

/* ---- comparing against the merged image ---- */

typedef struct { uint64_t offset, size; } file_row;
typedef struct {
    const tokc_game *g;
    int image_fd;
    file_row *rows; uint64_t nrows;
    _Atomic uint64_t next, bytes, bad, reads, across;
    uint64_t *bounds; uint64_t nbounds;
    uint64_t iterations, seed;
} job;

/* One compare of a range through both; the mismatch is reported by place only. */
static int same_range(job *j, uint8_t *a, uint8_t *b, uint64_t off, size_t n) {
    int64_t got = tokc_romfs_pread(j->g, a, n, off);
    if (got < 0 || (uint64_t)got != n) { fprintf(stderr, "read %#llx +%zu returned %lld\n", (ull)off, n, (long long)got); return 0; }
    if (tokc_pread_full(j->image_fd, b, n, off)) { fprintf(stderr, "image read %#llx +%zu failed\n", (ull)off, n); return 0; }
    if (memcmp(a, b, n)) {
        size_t i = 0;
        while (a[i] == b[i]) i++;
        fprintf(stderr, "differs at %#llx (read %#llx +%zu)\n", (ull)(off + i), (ull)off, n);
        return 0;
    }
    return 1;
}

static void *files_worker(void *arg) {
    job *j = arg;
    uint8_t *a = malloc(BLOCK), *b = malloc(BLOCK);
    for (;;) {
        uint64_t i = atomic_fetch_add(&j->next, 1);
        if (i >= j->nrows) break;
        const file_row *r = &j->rows[i];
        for (uint64_t done = 0; done < r->size;) {
            size_t n = r->size - done < BLOCK ? (size_t)(r->size - done) : BLOCK;
            if (!same_range(j, a, b, r->offset + done, n)) { atomic_fetch_add(&j->bad, 1); break; }
            done += n;
            atomic_fetch_add(&j->bytes, n);
        }
    }
    free(a); free(b);
    return NULL;
}

static void *image_worker(void *arg) {
    job *j = arg;
    uint8_t *a = malloc(BLOCK), *b = malloc(BLOCK);
    uint64_t size = tokc_romfs_size(j->g);
    for (;;) {
        uint64_t off = atomic_fetch_add(&j->next, BLOCK);
        if (off >= size) break;
        size_t n = size - off < BLOCK ? (size_t)(size - off) : BLOCK;
        if (!same_range(j, a, b, off, n)) atomic_fetch_add(&j->bad, 1);
        atomic_fetch_add(&j->bytes, n);
    }
    free(a); free(b);
    return NULL;
}

/* Random reads, a quarter of each kind: anywhere and any size; tiny and unaligned; across a place where the source or counter
   changes; past the end of the image. */
static void *random_worker(void *arg) {
    job *j = arg;
    enum { MAX = 8u << 20 };
    uint8_t *a = malloc(MAX), *b = malloc(MAX);
    uint64_t size = tokc_romfs_size(j->g), s = j->seed + 0x9E3779B97F4A7C15ull * (atomic_fetch_add(&j->next, 1) + 1);
    for (uint64_t i = 0; i < j->iterations; i++) {
        uint64_t r = rnd(&s), off;
        size_t n;
        switch (r & 3) {
        case 0:
            off = rnd(&s) % size;
            n = 1 + (size_t)(rnd(&s) % (1u << (1 + rnd(&s) % 23)));
            break;
        case 1:
            off = rnd(&s) % size;
            n = 1 + (size_t)(rnd(&s) % 40);
            break;
        case 2: {
            uint64_t at = j->bounds[rnd(&s) % j->nbounds], before = rnd(&s) % (1u << (1 + rnd(&s) % 20));
            off = at > before ? at - before : 0;
            n = (size_t)(at - off) + 1 + (size_t)(rnd(&s) % (1u << (1 + rnd(&s) % 20)));
            atomic_fetch_add(&j->across, 1);
            break;
        }
        default: {
            off = size - 1 - rnd(&s) % (1u << 20);
            n = 1 + (size_t)(rnd(&s) % (2u << 20));
            size_t want = size - off < n ? (size_t)(size - off) : n;
            int64_t got = tokc_romfs_pread(j->g, a, n, off);
            if (got != (int64_t)want || tokc_pread_full(j->image_fd, b, want, off) || memcmp(a, b, want)) {
                fprintf(stderr, "read at the end %#llx +%zu differs\n", (ull)off, n);
                atomic_fetch_add(&j->bad, 1);
            }
            if (tokc_romfs_pread(j->g, a, 16, size) != 0 || tokc_romfs_pread(j->g, a, 16, size + r % 4096) != 0) atomic_fetch_add(&j->bad, 1);
            atomic_fetch_add(&j->reads, 1);
            continue;
        }
        }
        if (n > MAX) n = MAX;
        if (off + n > size) n = (size_t)(size - off);
        if (!same_range(j, a, b, off, n)) atomic_fetch_add(&j->bad, 1);
        atomic_fetch_add(&j->reads, 1);
        atomic_fetch_add(&j->bytes, n);
    }
    free(a); free(b);
    return NULL;
}

static void run_threads(int n, void *(*fn)(void *), job *j) {
    pthread_t t[256];
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) if (pthread_create(&t[i], NULL, fn, j)) { fprintf(stderr, "cannot start a thread\n"); exit(2); }
    for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
}

/* The places in the image where a read has to change source or counter. */
static uint64_t *boundaries(const tokc_game *g, uint64_t *count) {
    const tokc_bktr *b = &g->bktr;
    uint64_t *out = malloc((b->nrel + b->nsub * 2 + 1) * 8 + 8), n = 0, cap = b->nrel + b->nsub * 2 + 1;
    for (uint64_t i = 0; i < b->nrel; i++) {
        uint64_t end = i + 1 < b->nrel ? b->virt[i + 1] : b->total;
        if (b->virt[i] > g->image_off && b->virt[i] - g->image_off < g->image_size && n < cap) out[n++] = b->virt[i] - g->image_off;
        if (!(b->phys[i] & TOKC_BKTR_PATCH)) continue;
        uint64_t phys = b->phys[i] & ~TOKC_BKTR_PATCH, len = end - b->virt[i];
        for (uint64_t k = 0; k < b->nsub; k++) {
            uint64_t s = b->sub_off[k];
            if (s <= phys || s >= phys + len) continue;
            uint64_t v = b->virt[i] + (s - phys);
            if (v > g->image_off && v - g->image_off < g->image_size && n < cap) out[n++] = v - g->image_off;
        }
    }
    *count = n;
    return out;
}

static file_row *parse_index(const uint8_t *idx, size_t size, uint64_t *count) {
    if (size < 24 || memcmp(idx, "TOTKRFS1", 8)) { fprintf(stderr, "not a TOTKRFS1 index\n"); exit(2); }
    uint64_t n = rd64(idx + 8);
    if (n > (size - 24) / 24) { fprintf(stderr, "index is cut short\n"); exit(2); }
    file_row *rows = malloc((size_t)(n ? n : 1) * sizeof *rows);
    for (uint64_t i = 0; i < n; i++) { rows[i].offset = rd64(idx + 24 + i * 24); rows[i].size = rd64(idx + 32 + i * 24); }
    *count = n;
    return rows;
}

/* The two indexes, field by field, when their bytes differ. */
static void explain_index(const uint8_t *a, size_t na, const uint8_t *b, size_t nb) {
    uint64_t ca = rd64(a + 8), cb = rd64(b + 8), sa = rd64(a + 16), sb = rd64(b + 16);
    printf("  sizes %zu and %zu, files %llu and %llu\n", na, nb, (ull)ca, (ull)cb);
    uint64_t off = 0, size = 0, name = 0, first = UINT64_MAX;
    for (uint64_t i = 0; i < ca && i < cb; i++) {
        const uint8_t *x = a + 24 + i * 24, *y = b + 24 + i * 24;
        int d = 0;
        if (rd64(x) != rd64(y)) { off++; d = 1; }
        if (rd64(x + 8) != rd64(y + 8)) { size++; d = 1; }
        if (rd32(x + 20) != rd32(y + 20) || strcmp((const char *)a + sa + rd32(x + 16), (const char *)b + sb + rd32(y + 16))) { name++; d = 1; }
        if (d && first == UINT64_MAX) first = i;
    }
    printf("  rows differing: offset %llu, size %llu, name %llu; first at row %lld\n", (ull)off, (ull)size, (ull)name, first == UINT64_MAX ? -1ll : (long long)first);
}

static int cmd_check(int argc, char **argv) {
    if (argc < 3) return 2;
    int threads = (int)sysconf(_SC_NPROCESSORS_ONLN), failed = 0;
    uint64_t seed = 1;
    char err[256];
    double t0 = now();
    tokc_keys *k = load_keys(argv[0]);
    int bfd = open_in(argv[1]), ufd = open_in(argv[2]);
    tokc_game *g;
    int rc = tokc_game_open(bfd, ufd, k, &g, err, sizeof err);
    tokc_keys_free(k);
    if (rc) { printf("FAIL open: error %d: %s\n", rc, err); return 1; }
    printf("open: %.1f ms, image %llu bytes, AES %s\n", (now() - t0) * 1e3, (ull)tokc_romfs_size(g), tokc_aes_impl_name());
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(a, "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--modules") && i + 1 < argc) {
            const char *dir = argv[++i];
            static const char *const names[] = {"main", "sdk", "subsdk0"};
            for (int m = 0; m < TOKC_MODULE_COUNT; m++) {
                uint8_t *bytes;
                size_t size, want_size;
                char path[4096];
                double t = now();
                rc = tokc_game_module(g, (tokc_module)m, &bytes, &size, err, sizeof err);
                t = now() - t;
                if (rc) { printf("FAIL module %s: %s\n", names[m], err); failed = 1; continue; }
                snprintf(path, sizeof path, "%s/%s", dir, names[m]);
                uint8_t *want = read_all(path, &want_size);
                int same = size == want_size && !memcmp(bytes, want, size);
                printf("%s module %s: %zu bytes, read and hashed in %.1f ms\n", same ? "PASS" : "FAIL", names[m], size, t * 1e3);
                failed |= !same;
                free(bytes); free(want);
            }
        } else if (!strcmp(a, "--index") && i + 1 < argc) {
            size_t want_size, size;
            uint8_t *want = read_all(argv[++i], &want_size), *bytes;
            double t = now();
            rc = tokc_romfs_index(g, &bytes, &size, err, sizeof err);
            t = now() - t;
            if (rc) { printf("FAIL index: %s\n", err); failed = 1; free(want); continue; }
            int same = size == want_size && !memcmp(bytes, want, size);
            printf("%s index: %llu files, %zu bytes, built in %.1f ms, %s\n", same ? "PASS" : "FAIL", (ull)rd64(bytes + 8), size, t * 1e3, same ? "byte for byte the same" : "differs:");
            if (!same) { explain_index(bytes, size, want, want_size); failed = 1; }
            free(bytes); free(want);
        } else if (!strcmp(a, "--files") && i + 2 < argc) {
            size_t size;
            uint8_t *idx = read_all(argv[++i], &size);
            job j;
            memset(&j, 0, sizeof j);
            j.g = g; j.image_fd = open_in(argv[++i]); j.rows = parse_index(idx, size, &j.nrows);
            double t = now();
            run_threads(threads, files_worker, &j);
            t = now() - t;
            printf("%s every file: %llu files, %llu bytes compared, %llu differ, %.1f s on %d threads\n", j.bad ? "FAIL" : "PASS", (ull)j.nrows, (ull)j.bytes, (ull)j.bad, t, threads);
            failed |= j.bad != 0;
            close(j.image_fd); free(j.rows); free(idx);
        } else if (!strcmp(a, "--image") && i + 1 < argc) {
            job j;
            memset(&j, 0, sizeof j);
            j.g = g; j.image_fd = open_in(argv[++i]);
            struct stat st;
            if (fstat(j.image_fd, &st) || (uint64_t)st.st_size != tokc_romfs_size(g)) { printf("FAIL whole image: sizes differ\n"); failed = 1; }
            double t = now();
            run_threads(threads, image_worker, &j);
            t = now() - t;
            printf("%s whole image: %llu bytes compared, %llu blocks differ, %.1f s on %d threads\n", j.bad ? "FAIL" : "PASS", (ull)j.bytes, (ull)j.bad, t, threads);
            failed |= j.bad != 0;
            close(j.image_fd);
        } else if (!strcmp(a, "--random") && i + 2 < argc) {
            job j;
            memset(&j, 0, sizeof j);
            uint64_t total = strtoull(argv[++i], NULL, 0);
            j.g = g; j.image_fd = open_in(argv[++i]); j.seed = seed; j.iterations = total / (uint64_t)threads + 1;
            j.bounds = boundaries(g, &j.nbounds);
            double t = now();
            run_threads(threads, random_worker, &j);
            t = now() - t;
            printf("%s random reads: %llu reads, %llu aimed across %llu boundaries, %llu bytes, %llu differ, %.1f s on %d threads, seed %llu\n",
                   j.bad ? "FAIL" : "PASS", (ull)j.reads, (ull)j.across, (ull)j.nbounds, (ull)j.bytes, (ull)j.bad, t, threads, (ull)seed);
            failed |= j.bad != 0;
            close(j.image_fd); free(j.bounds);
        } else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    tokc_stats s;
    tokc_game_stats(g, &s);
    printf("tables: %llu relocations, %llu subsections, %llu bytes held; section from base %llu bytes, from update %llu bytes\n",
           (ull)s.relocations, (ull)s.subsections, (ull)s.table_bytes, (ull)s.base_bytes, (ull)s.update_bytes);
    tokc_game_close(g);
    close(bfd); close(ufd);
    return failed;
}

/* ---- timing ---- */

typedef struct { uint64_t off; uint32_t n; } read_op;
typedef struct {
    /* NULL: plain pread on image_fd */
    const tokc_game *g;
    int image_fd;
    const read_op *ops; uint64_t nops;
    /* per op */
    uint64_t *ns;
    _Atomic uint64_t next, runs;
    /* sequential: end of the range */
    uint64_t stop_at;
    uint32_t block;
    _Atomic uint64_t bytes;
    int failed;
} bench_job;

static int bench_read(const bench_job *j, void *buf, size_t n, uint64_t off) {
    if (j->g) return tokc_romfs_pread(j->g, buf, n, off) == (int64_t)n ? 0 : -1;
    return tokc_pread_full(j->image_fd, buf, n, off);
}

static void *seq_worker(void *arg) {
    bench_job *j = arg;
    uint8_t *buf = malloc(j->block);
    for (;;) {
        uint64_t off = atomic_fetch_add(&j->next, j->block);
        if (off >= j->stop_at) break;
        size_t n = j->stop_at - off < j->block ? (size_t)(j->stop_at - off) : j->block;
        if (bench_read(j, buf, n, off)) { j->failed = 1; break; }
        atomic_fetch_add(&j->bytes, n);
    }
    free(buf);
    return NULL;
}

static void *ops_worker(void *arg) {
    bench_job *j = arg;
    uint32_t cap = 0;
    for (uint64_t i = 0; i < j->nops; i++) if (j->ops[i].n > cap) cap = j->ops[i].n;
    uint8_t *buf = malloc(cap);
    for (;;) {
        uint64_t i = atomic_fetch_add(&j->next, 1);
        if (i >= j->nops) break;
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        if (bench_read(j, buf, j->ops[i].n, j->ops[i].off)) { j->failed = 1; break; }
        clock_gettime(CLOCK_MONOTONIC, &b);
        j->ns[i] = (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull + (uint64_t)b.tv_nsec - (uint64_t)a.tv_nsec;
    }
    free(buf);
    return NULL;
}

static int by_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }

static void drop_cache(int fd) { posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED); }

typedef struct { const tokc_game *g; int image_fd, base_fd, update_fd, threads, cold; } rig;

static void drop_all(const rig *r) {
    if (!r->cold) return;
    drop_cache(r->base_fd); drop_cache(r->update_fd);
    if (r->image_fd >= 0) drop_cache(r->image_fd);
}

static double seq_run(const rig *r, int use_reader, int threads, uint32_t block, uint64_t bytes) {
    bench_job j;
    memset(&j, 0, sizeof j);
    j.g = use_reader ? r->g : NULL; j.image_fd = r->image_fd; j.block = block; j.stop_at = bytes;
    pthread_t t[64];
    drop_all(r);
    double t0 = now();
    if (threads > 64) threads = 64;
    for (int i = 0; i < threads; i++) pthread_create(&t[i], NULL, seq_worker, &j);
    for (int i = 0; i < threads; i++) pthread_join(t[i], NULL);
    t0 = now() - t0;
    if (j.failed) printf("  a read failed\n");
    return (double)j.bytes / t0 / 1e6;
}

static void ops_run(const rig *r, int use_reader, int threads, const read_op *ops, uint64_t n, const char *label) {
    bench_job j;
    memset(&j, 0, sizeof j);
    j.g = use_reader ? r->g : NULL; j.image_fd = r->image_fd; j.ops = ops; j.nops = n; j.ns = calloc((size_t)n, 8);
    pthread_t t[64];
    if (threads > 64) threads = 64;
    drop_all(r);
    double t0 = 0;
    /* warm: a first pass brings every byte into the page cache, the second is the one timed */
    for (int pass = r->cold; pass < 2; pass++) {
        atomic_store(&j.next, 0);
        t0 = now();
        for (int i = 0; i < threads; i++) pthread_create(&t[i], NULL, ops_worker, &j);
        for (int i = 0; i < threads; i++) pthread_join(t[i], NULL);
        t0 = now() - t0;
    }
    if (j.failed) printf("  a read failed\n");
    uint64_t bytes = 0;
    for (uint64_t i = 0; i < n; i++) bytes += ops[i].n;
    qsort(j.ns, (size_t)n, 8, by_u64);
    printf("  %-22s %2d thr  p50 %8.1f  p95 %8.1f  p99 %8.1f  max %9.1f us   %7.0f MB/s\n", label, threads, (double)j.ns[n / 2] / 1e3, (double)j.ns[n * 95 / 100] / 1e3,
           (double)j.ns[n * 99 / 100] / 1e3, (double)j.ns[n - 1] / 1e3, (double)bytes / t0 / 1e6);
    free(j.ns);
}

/* Reads of one fixed size at random places, or with size 0 random files: whole up to 8 MB, 128 KB steps of a stream above.
   The sizes follow TOTK_FS_TIMING in play: a mean read of 82 KB, per-second means from 16 KB to 1 MB, 128 KB reads of streams. */
static read_op *make_ops(const tokc_game *g, const file_row *rows, uint64_t nrows, uint32_t size, uint64_t n, uint64_t seed, double *mean) {
    read_op *ops = malloc((size_t)n * sizeof *ops);
    uint64_t s = seed * 2 + 1, total = 0, image = tokc_romfs_size(g);
    for (uint64_t i = 0; i < n; i++) {
        if (size) {
            ops[i].n = size;
            ops[i].off = rnd(&s) % (image - size);
        } else {
            const file_row *r;
            do r = &rows[rnd(&s) % nrows]; while (!r->size);
            if (r->size <= (8u << 20)) { ops[i].n = (uint32_t)r->size; ops[i].off = r->offset; }
            else { ops[i].n = 128u << 10; ops[i].off = r->offset + (rnd(&s) % (r->size >> 17)) * (128u << 10); }
        }
        total += ops[i].n;
    }
    *mean = (double)total / (double)n;
    return ops;
}

static double aes_speed(int mode) {
    enum { N = 32u << 20 };
    uint8_t key[16] = {1}, high[8] = {2}, *p = calloc(1, N);
    tokc_aes a;
    tokc_aes_init(&a, key, mode);
    double t = now(), done = 0;
    while (now() - t < 0.5) { tokc_aes_ctr(&a, high, 0, p, N); done += N; }
    t = now() - t;
    free(p);
    return done / t / 1e6;
}

static int cmd_bench(int argc, char **argv) {
    if (argc < 3) return 2;
    rig r = {NULL, -1, -1, -1, (int)sysconf(_SC_NPROCESSORS_ONLN), 0};
    uint64_t seed = 1, samples = 20000, seq_bytes = 4ull << 30;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--threads") && i + 1 < argc) r.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) r.image_fd = open_in(argv[++i]);
        else if (!strcmp(argv[i], "--cold")) r.cold = 1;
        else if (!strcmp(argv[i], "--samples") && i + 1 < argc) samples = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seq-gb") && i + 1 < argc) seq_bytes = strtoull(argv[++i], NULL, 0) << 30;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    char err[256];
    r.base_fd = open_in(argv[1]); r.update_fd = open_in(argv[2]);
    double t = now();
    tokc_keys *k = load_keys(argv[0]);
    tokc_game *g;
    int rc = tokc_game_open(r.base_fd, r.update_fd, k, &g, err, sizeof err);
    tokc_keys_free(k);
    if (rc) { printf("open: error %d: %s\n", rc, err); return 1; }
    r.g = g;
    printf("keys and open: %.1f ms\n", (now() - t) * 1e3);
    uint8_t *idx;
    size_t idx_size;
    t = now();
    if ((rc = tokc_romfs_index(g, &idx, &idx_size, err, sizeof err))) { printf("index: %s\n", err); return 1; }
    printf("index: %.1f ms, %zu bytes\n", (now() - t) * 1e3, idx_size);
    uint64_t nrows;
    file_row *rows = parse_index(idx, idx_size, &nrows);
    tokc_stats s;
    tokc_game_stats(g, &s);
    printf("tables held: %llu bytes (%llu relocations, %llu subsections)\n", (ull)s.table_bytes, (ull)s.relocations, (ull)s.subsections);
    printf("AES-CTR in memory, one thread: %s %.0f MB/s, portable %.0f MB/s\n", tokc_aes_impl_name(), aes_speed(TOKC_AES_AUTO), aes_speed(TOKC_AES_PORTABLE));
    if (seq_bytes > tokc_romfs_size(g)) seq_bytes = tokc_romfs_size(g);
    printf("sequential, first %.1f GB of the image, %s cache:\n", (double)seq_bytes / 1e9, r.cold ? "cold" : "as found");
    static const uint32_t blocks[] = {128u << 10, 1u << 20, 8u << 20};
    for (int b = 0; b < 3; b++) {
        int tc[2] = {1, r.threads};
        for (int i = 0; i < 2; i++) {
            printf("  %5u KB blocks, %2d thr: reader %7.0f MB/s", blocks[b] >> 10, tc[i], seq_run(&r, 1, tc[i], blocks[b], seq_bytes));
            if (r.image_fd >= 0) printf(", image %7.0f MB/s", seq_run(&r, 0, tc[i], blocks[b], seq_bytes));
            printf("\n");
        }
    }
    printf("random reads, up to %llu of each kind and 2 GB, %s:\n", (ull)samples, r.cold ? "cold cache" : "all in the page cache");
    static const uint32_t sizes[] = {4u << 10, 16u << 10, 64u << 10, 128u << 10, 256u << 10, 1u << 20, 0};
    for (int i = 0; i < 7; i++) {
        double mean;
        uint64_t count = sizes[i] && samples > (2ull << 30) / sizes[i] ? (2ull << 30) / sizes[i] : samples;
        read_op *ops = make_ops(g, rows, nrows, sizes[i], count, seed + (uint64_t)i, &mean);
        uint64_t runs = 0;
        for (uint64_t k2 = 0; k2 < count; k2++) runs += tokc_romfs_runs(g, ops[k2].off, ops[k2].n);
        char label[40];
        if (sizes[i]) snprintf(label, sizeof label, "%u KB", sizes[i] >> 10);
        else snprintf(label, sizeof label, "files, mean %.0f KB", mean / 1024);
        printf(" %s: %.3f source runs per read\n", label, (double)runs / (double)count);
        int tc[2] = {1, r.threads};
        for (int c = 0; c < 2; c++) {
            ops_run(&r, 1, tc[c], ops, count, "reader");
            if (r.image_fd >= 0) ops_run(&r, 0, tc[c], ops, count, "image");
        }
        free(ops);
    }
    free(rows); free(idx);
    tokc_game_close(g);
    return 0;
}

int main(int argc, char **argv) {
    /* key material must not reach a core file, even through a piped core handler */
    struct rlimit none = {0, 0};
    setrlimit(RLIMIT_CORE, &none);
    prctl(PR_SET_DUMPABLE, 0);
    if (argc >= 2 && !strcmp(argv[1], "selftest")) return tokc_selftest() != 0;
    int rc = 2;
    if (argc >= 2 && !strcmp(argv[1], "info")) rc = cmd_info(argc - 2, argv + 2);
    else if (argc >= 2 && !strcmp(argv[1], "check")) rc = cmd_check(argc - 2, argv + 2);
    else if (argc >= 2 && !strcmp(argv[1], "bench")) rc = cmd_bench(argc - 2, argv + 2);
    if (rc == 2) fprintf(stderr, "usage: container_cli selftest | info KEYS FILE... | check KEYS BASE UPDATE ... | bench KEYS BASE UPDATE ... (see container_cli.c)\n");
    return rc;
}

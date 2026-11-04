/* The reader on made-up files: everything it must read, and every refusal it must give, with the error it must give.
     test_synth        exit 0: all passed */
#define _GNU_SOURCE 1
#include "synth.h"
#include "selftest.h"
#include "../container_internal.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static tokc_keys *keys_of(const sy_game *g) {
    tokc_keys *k = NULL;
    char err[200] = "";
    int rc = tokc_keys_parse(g->keys_text, g->keys_size, &k, err, sizeof err);
    CHECK(rc == TOKC_OK, "keys: %s", err);
    return k;
}

typedef struct { const tokc_game *g; const sy_game *s; uint64_t seed; int bad; } reader;

static void *read_loop(void *arg) {
    reader *r = arg;
    uint64_t st = r->seed * 2 + 1;
    size_t size = r->s->image_size;
    for (int i = 0; i < 4000; i++) {
        st ^= st << 13; st ^= st >> 7; st ^= st << 17;
        uint64_t off = st % (size + 40);
        size_t n = (size_t)(st >> 32) % (i & 1 ? 40 : 9000);
        uint8_t *b = malloc(n ? n : 1);
        int64_t got = tokc_romfs_pread(r->g, b, n, off);
        size_t want = off >= size ? 0 : n < size - off ? n : (size_t)(size - off);
        if (got != (int64_t)want || (want && memcmp(b, r->s->image + off, want))) r->bad++;
        free(b);
    }
    return NULL;
}

/* Everything an accepted pair must give. */
static void full_read(const sy_spec *spec, const char *what) {
    sy_game s;
    sy_make(&s, spec);
    tokc_keys *k = keys_of(&s);
    int bf = sy_fd(s.base.data, s.base.size), uf = sy_fd(s.update.data, s.update.size);
    tokc_game *g = NULL;
    char err[200] = "";
    int rc = tokc_game_open(bf, uf, k, &g, err, sizeof err);
    CHECK(rc == TOKC_OK, "%s: open: %d %s", what, rc, err);
    tokc_keys_free(k);
    if (rc) goto out;
    CHECK(tokc_romfs_size(g) == s.image_size, "%s: size", what);
    for (int m = 0; m < 3; m++) {
        uint8_t *b;
        size_t n;
        rc = tokc_game_module(g, (tokc_module)m, &b, &n, err, sizeof err);
        CHECK(!rc && n == s.module_size[m] && !memcmp(b, s.module[m], n), "%s: module %d: %s", what, m, err);
        if (!rc) free(b);
    }
    uint8_t *idx;
    size_t idx_size;
    rc = tokc_romfs_index(g, &idx, &idx_size, err, sizeof err);
    CHECK(!rc && idx_size == s.index_size && !memcmp(idx, s.index, idx_size), "%s: index: %s", what, err);
    if (!rc) free(idx);
    uint8_t *all = malloc(s.image_size);
    CHECK(tokc_romfs_pread(g, all, s.image_size, 0) == (int64_t)s.image_size && !memcmp(all, s.image, s.image_size), "%s: whole image", what);
    free(all);
    uint8_t one;
    CHECK(tokc_romfs_pread(g, &one, 1, s.image_size) == 0 && tokc_romfs_pread(g, &one, 1, UINT64_MAX) == 0, "%s: past the end", what);
    CHECK(tokc_romfs_pread(g, NULL, 0, 0) == 0 && tokc_romfs_pread(g, NULL, 1, 0) == TOKC_E_ARGS, "%s: empty reads", what);
    /* several threads on one game, against the plain image */
    reader r[8];
    pthread_t t[8];
    for (int i = 0; i < 8; i++) { r[i] = (reader){g, &s, spec->seed + (uint64_t)i + 1, 0}; pthread_create(&t[i], NULL, read_loop, &r[i]); }
    int bad = 0;
    for (int i = 0; i < 8; i++) { pthread_join(t[i], NULL); bad += r[i].bad; }
    CHECK(!bad, "%s: %d threaded reads differ", what, bad);
    tokc_stats st;
    tokc_game_stats(g, &st);
    CHECK(st.relocations == spec->pieces && st.subsections == spec->subsections, "%s: stats", what);
    tokc_game_close(g);
out:
    close(bf); close(uf);
    sy_free(&s);
}

/* Opens the pair and wants this error, and its text to hold the given words. */
static void want_error(const sy_game *s, const void *keys, size_t keys_size, const uint8_t *base, size_t base_size, const uint8_t *update, size_t update_size, int code, const char *words, const char *what) {
    tokc_keys *k = NULL;
    char err[200] = "";
    int rc = tokc_keys_parse(keys, keys_size, &k, err, sizeof err);
    (void)s;
    if (!rc) {
        int bf = sy_fd(base, base_size), uf = sy_fd(update, update_size);
        tokc_game *g = NULL;
        rc = tokc_game_open(bf, uf, k, &g, err, sizeof err);
        tokc_game_close(g);
        close(bf); close(uf);
        tokc_keys_free(k);
    }
    CHECK(rc == code, "%s: error %d, wanted %d (%s)", what, rc, code, err);
    CHECK(!words || strstr(err, words), "%s: text \"%s\" lacks \"%s\"", what, err, words);
}
#define ERROR_OF(s, code, words, what) want_error(s, (s)->keys_text, (s)->keys_size, (s)->base.data, (s)->base.size, (s)->update.data, (s)->update.size, code, words, what)

static void identity_errors(void) {
    sy_spec spec;
    sy_game s;
    sy_default(&spec, 2);
    sy_make(&s, &spec);
    want_error(&s, s.keys_text, s.keys_size, s.update.data, s.update.size, s.base.data, s.base.size, TOKC_E_SWAPPED, "is the update", "base and update swapped");
    want_error(&s, s.keys_text, s.keys_size, s.base.data, s.base.size, s.base.data, s.base.size, TOKC_E_SWAPPED, "is the base game", "the base given twice");
    want_error(&s, s.keys_text, s.keys_size, s.update.data, s.update.size, s.update.data, s.update.size, TOKC_E_SWAPPED, "is the update", "the update given twice");
    sy_free(&s);
    sy_default(&spec, 3); spec.update_version = 65536;
    sy_make(&s, &spec); ERROR_OF(&s, TOKC_E_WRONG_VERSION, "not 1.4.3", "old update"); sy_free(&s);
    sy_default(&spec, 3); spec.base_version = 65536;
    sy_make(&s, &spec); ERROR_OF(&s, TOKC_E_WRONG_VERSION, "not 1.0.0", "newer base"); sy_free(&s);
    sy_default(&spec, 4); spec.base_title = 0x0100000000010000ull;
    sy_make(&s, &spec); ERROR_OF(&s, TOKC_E_WRONG_GAME, "not Tears of the Kingdom", "another game as base"); sy_free(&s);
    sy_default(&spec, 4); spec.update_title = 0x0100000000010800ull;
    sy_make(&s, &spec); ERROR_OF(&s, TOKC_E_WRONG_GAME, "not for Tears of the Kingdom", "another game as update"); sy_free(&s);
}

/* The text with the line naming a key taken out, or its value changed. */
static char *edit_keys(const sy_game *s, const char *name, int change, size_t *size) {
    char *t = malloc(s->keys_size + 1);
    memcpy(t, s->keys_text, s->keys_size);
    t[s->keys_size] = 0;
    char *at = strcasestr(t, name), *nl = strchr(at, '\n');
    if (change) {
        char *eq = strchr(at, '=');
        eq[2] = eq[2] == 'f' ? '0' : 'f';
        *size = s->keys_size;
    } else {
        memmove(at, nl + 1, strlen(nl + 1) + 1);
        *size = strlen(t);
    }
    return t;
}

static void key_errors(void) {
    sy_spec spec;
    sy_game s;
    size_t n;
    char *t;
    sy_default(&spec, 5);
    sy_make(&s, &spec);
    t = edit_keys(&s, "titlekek_0e", 0, &n);
    want_error(&s, t, n, s.base.data, s.base.size, s.update.data, s.update.size, TOKC_E_KEY_MISSING, "titlekek_0e", "no title KEK"); free(t);
    t = edit_keys(&s, "key_area_key_application_0e", 0, &n);
    want_error(&s, t, n, s.base.data, s.base.size, s.update.data, s.update.size, TOKC_E_KEY_MISSING, "key_area_key_application_0e", "no key area key"); free(t);
    t = edit_keys(&s, "header_key", 0, &n);
    want_error(&s, t, n, s.base.data, s.base.size, s.update.data, s.update.size, TOKC_E_KEY_MISSING, "header_key", "no header key"); free(t);
    t = edit_keys(&s, "header_key", 1, &n);
    want_error(&s, t, n, s.base.data, s.base.size, s.update.data, s.update.size, TOKC_E_KEY_WRONG, "header_key", "wrong header key"); free(t);
    t = edit_keys(&s, "titlekek_0e", 1, &n);
    want_error(&s, t, n, s.base.data, s.base.size, s.update.data, s.update.size, TOKC_E_KEY_WRONG, "do not decrypt", "wrong title KEK"); free(t);
    t = edit_keys(&s, "key_area_key_application_0e", 1, &n);
    want_error(&s, t, n, s.base.data, s.base.size, s.update.data, s.update.size, TOKC_E_KEY_WRONG, "do not decrypt", "wrong key area key"); free(t);
    /* a bad value is named, and not repeated */
    char text[] = "header_key = 0011\n";
    char err[200] = "";
    tokc_keys *k;
    int rc = tokc_keys_parse(text, strlen(text), &k, err, sizeof err);
    CHECK(rc == TOKC_E_FORMAT && strstr(err, "header_key") && !strstr(err, "0011"), "short value: %d %s", rc, err);
    char big[] = "titlekek_ff = 00112233445566778899aabbccddeeff\nmaster_key_40 = 00112233445566778899aabbccddeeff\n";
    rc = tokc_keys_parse(big, strlen(big), &k, err, sizeof err);
    CHECK(rc == TOKC_E_KEY_MISSING, "generations past the table are ignored: %d %s", rc, err);
    sy_free(&s);
}

static void personal(void *user, sy_region r, uint8_t *data, size_t size) {
    (void)size;
    if (r == SY_TICKET) data[0x140 + 0x141] = *(int *)user;
}
static void no_ticket(void *user, sy_region r, uint8_t *data, size_t size) {
    (void)user;
    if (r == SY_TICKET) memset(data, 0, size);
}

static void ticket_errors(void) {
    sy_spec spec;
    sy_game s;
    int one = 1;
    sy_default(&spec, 6); spec.hook = personal; spec.user = &one;
    sy_make(&s, &spec); ERROR_OF(&s, TOKC_E_TICKET, "common ticket", "personalized ticket"); sy_free(&s);
    sy_default(&spec, 6); spec.hook = no_ticket;
    sy_make(&s, &spec); ERROR_OF(&s, TOKC_E_TICKET, "no ticket", "no ticket"); sy_free(&s);
}

static void file_errors(void) {
    sy_spec spec;
    sy_game s;
    sy_default(&spec, 7);
    sy_make(&s, &spec);
    static const uint8_t junk[64] = "not a game file";
    want_error(&s, s.keys_text, s.keys_size, junk, sizeof junk, s.update.data, s.update.size, TOKC_E_FORMAT, "not an NSP or XCI", "junk base");
    want_error(&s, s.keys_text, s.keys_size, junk, 0, s.update.data, s.update.size, TOKC_E_FORMAT, "too small", "empty base");
    want_error(&s, s.keys_text, s.keys_size, s.base.data, s.base.size, s.update.data, s.update.size / 2, TOKC_E_FORMAT, NULL, "update cut in half");
    want_error(&s, s.keys_text, s.keys_size, s.base.data, s.base.size / 3, s.update.data, s.update.size, TOKC_E_FORMAT, NULL, "base cut short");
    tokc_keys *k = keys_of(&s);
    char err[200];
    tokc_info info;
    int dir = open("/", O_RDONLY | O_DIRECTORY), bf = sy_fd(s.base.data, s.base.size), uf = sy_fd(s.update.data, s.update.size);
    CHECK(tokc_inspect(dir, k, &info, err, sizeof err) == TOKC_E_IO, "a folder");
    CHECK(tokc_inspect(-1, k, &info, err, sizeof err) == TOKC_E_IO, "a bad descriptor");
    int rc = tokc_inspect(bf, k, &info, err, sizeof err);
    CHECK(!rc && info.type == TOKC_META_APPLICATION && info.title_id == TOKC_TITLE_BASE && info.version == 0, "inspect base");
    rc = tokc_inspect(uf, k, &info, err, sizeof err);
    CHECK(!rc && info.type == TOKC_META_PATCH && info.title_id == TOKC_TITLE_UPDATE && info.version == TOKC_VERSION_UPDATE, "inspect update");
    close(dir); close(bf); close(uf);
    tokc_keys_free(k);
    sy_free(&s);
}

/* Compressed packages get their own message: the NCA header says more than the entry holds, and a section table follows. */
static void compressed_file(void) {
    sy_spec spec;
    sy_game s;
    sy_default(&spec, 9); spec.pieces = 2000;
    sy_make(&s, &spec);
    uint8_t *f = s.update.data;
    uint32_t n = rd32(f + 4), strings = rd32(f + 8);
    uint64_t data = 16 + (uint64_t)n * 24 + strings;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *e = f + 16 + 24 * i;
        if (rd64(e + 8) < 0x8000) continue;
        memcpy(f + data + rd64(e) + 0x4000, "NCZSECTN", 8);
        wr64(e + 8, rd64(e + 8) - 1);
    }
    ERROR_OF(&s, TOKC_E_FORMAT, "compressed", "compressed package");
    sy_free(&s);
}

static void keys_from_descriptor(void) {
    sy_spec spec;
    sy_game s;
    sy_default(&spec, 10);
    sy_make(&s, &spec);
    int fd = sy_fd(s.keys_text, s.keys_size);
    tokc_keys *k = NULL;
    char err[200] = "";
    CHECK(tokc_keys_parse_fd(fd, &k, err, sizeof err) == TOKC_OK, "keys from a descriptor: %s", err);
    tokc_keys_free(k);
    close(fd);
    CHECK(tokc_keys_parse_fd(-1, &k, err, sizeof err) == TOKC_E_IO, "keys from a bad descriptor");
    sy_free(&s);
}

/* The other file behind the descriptor changes after open: the reader must report an error, not crash. */
static void shrink_after_open(void) {
    sy_spec spec;
    sy_game s;
    sy_default(&spec, 8);
    sy_make(&s, &spec);
    tokc_keys *k = keys_of(&s);
    int bf = sy_fd(s.base.data, s.base.size), uf = sy_fd(s.update.data, s.update.size);
    tokc_game *g;
    char err[200] = "";
    int rc = tokc_game_open(bf, uf, k, &g, err, sizeof err);
    CHECK(!rc, "open: %s", err);
    if (!rc) {
        if (ftruncate(bf, 0x1000) || ftruncate(uf, 0x1000)) abort();
        uint8_t *b = malloc(s.image_size);
        CHECK(tokc_romfs_pread(g, b, s.image_size, 0) == TOKC_E_IO, "read after truncation");
        free(b);
        uint8_t *m;
        size_t n;
        CHECK(tokc_game_module(g, TOKC_MODULE_MAIN, &m, &n, err, sizeof err) == TOKC_E_IO, "module after truncation");
        tokc_game_close(g);
    }
    close(bf); close(uf);
    tokc_keys_free(k);
    sy_free(&s);
}

int main(void) {
    if (tokc_selftest()) fails++;
    sy_spec spec;
    for (uint64_t seed = 1; seed <= 20; seed++) {
        sy_default(&spec, seed);
        full_read(&spec, "NSP");
        sy_default(&spec, seed); spec.xci = 1;
        full_read(&spec, "XCI");
    }
    sy_default(&spec, 21); spec.pieces = 2000; spec.subsections = 1500;
    full_read(&spec, "second buckets");
    sy_default(&spec, 22); spec.pieces = 1; spec.subsections = 1;
    full_read(&spec, "one piece");
    identity_errors();
    key_errors();
    ticket_errors();
    file_errors();
    compressed_file();
    keys_from_descriptor();
    shrink_after_open();
    printf("%s made-up files: %d checks passed, %d failed\n", fails ? "FAIL" : "PASS", passes, fails);
    return fails != 0;
}

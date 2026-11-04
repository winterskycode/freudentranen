/* prod.keys, in memory only: "name = hex" lines, names in any case, '#' and ';' start a comment. Only the keys a game needs
   are kept. Text here names a key, never its value. */
#include "container_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_KEYS_FILE (1u << 20)

/* The keys other keys are made from, while parsing. */
typedef struct {
    uint8_t titlekek[16], kek_generation[16], key_generation[16], area_app[16], header_kek[16], header_key[32];
    int have_titlekek, have_kek_generation, have_key_generation, have_area_app, have_header_kek, have_header_key;
} sources;

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static int parse_hex(const char *s, size_t len, uint8_t *out, size_t n) {
    if (len != 2 * n) return -1;
    for (size_t i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return 0;
}

/* The generation of name when it is prefix + two hex digits, else -1. */
static int generation(const char *name, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(name, prefix, n) || strlen(name) != n + 2) return -1;
    int a = hexval(name[n]), b = hexval(name[n + 1]);
    return a < 0 || b < 0 ? -1 : a << 4 | b;
}

static int key_line(tokc_keys *k, sources *s, const char *name, const char *v, size_t len) {
    int g;
    uint8_t *dst = NULL; size_t n = 16; int *flag = NULL; uint32_t *mask = NULL;
    if (!strcmp(name, "header_key")) { dst = k->header; n = 32; flag = &k->have_header; }
    else if (!strcmp(name, "titlekek_source")) { dst = s->titlekek; flag = &s->have_titlekek; }
    else if (!strcmp(name, "aes_kek_generation_source")) { dst = s->kek_generation; flag = &s->have_kek_generation; }
    else if (!strcmp(name, "aes_key_generation_source")) { dst = s->key_generation; flag = &s->have_key_generation; }
    else if (!strcmp(name, "key_area_key_application_source")) { dst = s->area_app; flag = &s->have_area_app; }
    else if (!strcmp(name, "header_kek_source")) { dst = s->header_kek; flag = &s->have_header_kek; }
    else if (!strcmp(name, "header_key_source")) { dst = s->header_key; n = 32; flag = &s->have_header_key; }
    else if ((g = generation(name, "titlekek_")) >= 0) { if (g >= TOKC_GENERATIONS) return 0; dst = k->titlekek[g]; mask = &k->have_titlekek; }
    else if ((g = generation(name, "key_area_key_application_")) >= 0) { if (g >= TOKC_GENERATIONS) return 0; dst = k->area_app[g]; mask = &k->have_area_app; }
    else if ((g = generation(name, "master_key_")) >= 0) { if (g >= TOKC_GENERATIONS) return 0; dst = k->master[g]; mask = &k->have_master; }
    else return 0;
    if (parse_hex(v, len, dst, n)) return -1;
    if (flag) *flag = 1;
    if (mask) *mask |= 1u << g;
    return 0;
}

/* The console's key derivation, for a key file that has master keys and sources but not the result. */
static void make_kek(uint8_t out[16], const uint8_t source[16], const uint8_t master[16], const sources *s) {
    uint8_t kek[16], src_kek[16];
    tokc_aes_ecb_decrypt(master, s->kek_generation, kek, 16);
    tokc_aes_ecb_decrypt(kek, source, src_kek, 16);
    tokc_aes_ecb_decrypt(src_kek, s->key_generation, out, 16);
    tokc_wipe(kek, 16); tokc_wipe(src_kek, 16);
}

static void derive(tokc_keys *k, const sources *s) {
    int seeds = s->have_kek_generation && s->have_key_generation;
    for (int g = 0; g < TOKC_GENERATIONS; g++) {
        if (!(k->have_master >> g & 1)) continue;
        if (!(k->have_titlekek >> g & 1) && s->have_titlekek) {
            tokc_aes_ecb_decrypt(k->master[g], s->titlekek, k->titlekek[g], 16);
            k->have_titlekek |= 1u << g;
        }
        if (!(k->have_area_app >> g & 1) && s->have_area_app && seeds) {
            make_kek(k->area_app[g], s->area_app, k->master[g], s);
            k->have_area_app |= 1u << g;
        }
    }
    if (!k->have_header && (k->have_master & 1) && s->have_header_kek && s->have_header_key && seeds) {
        uint8_t kek[16];
        make_kek(kek, s->header_kek, k->master[0], s);
        tokc_aes_ecb_decrypt(kek, s->header_key, k->header, 32);
        tokc_wipe(kek, 16);
        k->have_header = 1;
    }
}

static int blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

int tokc_keys_parse(const void *text, size_t size, tokc_keys **out, char *err, size_t errlen) {
    if (!text || !out) return tokc_fail(err, errlen, TOKC_E_ARGS, "no key text");
    *out = NULL;
    if (size > MAX_KEYS_FILE) return tokc_fail(err, errlen, TOKC_E_FORMAT, "prod.keys is too large to be a key file");
    tokc_keys *k = calloc(1, sizeof *k);
    if (!k) return tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory");
    sources s;
    memset(&s, 0, sizeof s);
    const char *t = text;
    size_t pos = size >= 3 && !memcmp(t, "\xef\xbb\xbf", 3) ? 3 : 0;
    int line = 0, rc = TOKC_OK;
    while (pos < size) {
        const char *nl = memchr(t + pos, '\n', size - pos);
        size_t end = nl ? (size_t)(nl - t) : size, stop = pos;
        line++;
        while (stop < end && t[stop] != '#' && t[stop] != ';') stop++;
        const char *eq = memchr(t + pos, '=', stop - pos);
        if (eq) {
            const char *a = t + pos, *b = eq, *v = eq + 1, *ve = t + stop;
            while (a < b && blank(*a)) a++;
            while (b > a && blank(b[-1])) b--;
            while (v < ve && blank(*v)) v++;
            while (ve > v && blank(ve[-1])) ve--;
            char name[64];
            size_t n = (size_t)(b - a);
            if (n && n < sizeof name) {
                for (size_t i = 0; i < n; i++) name[i] = (char)(a[i] >= 'A' && a[i] <= 'Z' ? a[i] + 32 : a[i]);
                name[n] = 0;
                if (key_line(k, &s, name, v, (size_t)(ve - v))) {
                    rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "prod.keys line %d: %s is not the right length of hex", line, name);
                    break;
                }
            }
        }
        pos = end + 1;
    }
    if (!rc) {
        derive(k, &s);
        if (!k->have_header) rc = tokc_fail(err, errlen, TOKC_E_KEY_MISSING, "prod.keys has no header_key");
    }
    tokc_wipe(&s, sizeof s);
    if (rc) { tokc_keys_free(k); return rc; }
    *out = k;
    return TOKC_OK;
}

int tokc_keys_parse_fd(int fd, tokc_keys **out, char *err, size_t errlen) {
    struct stat st;
    if (!out) return tokc_fail(err, errlen, TOKC_E_ARGS, "no output");
    *out = NULL;
    if (fstat(fd, &st) || st.st_size < 0) return tokc_fail(err, errlen, TOKC_E_IO, "prod.keys: cannot read: %s", strerror(errno));
    if ((uint64_t)st.st_size > MAX_KEYS_FILE) return tokc_fail(err, errlen, TOKC_E_FORMAT, "prod.keys is too large to be a key file");
    size_t n = (size_t)st.st_size;
    char *text = malloc(n ? n : 1);
    if (!text) return tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory");
    int rc = tokc_pread_full(fd, text, n, 0);
    if (rc) rc = tokc_fail(err, errlen, rc, "prod.keys: cannot read");
    else rc = tokc_keys_parse(text, n, out, err, errlen);
    tokc_wipe(text, n);
    free(text);
    return rc;
}

void tokc_keys_free(tokc_keys *k) {
    if (!k) return;
    tokc_wipe(k, sizeof *k);
    free(k);
}

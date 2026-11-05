/* What the comparison with the Python tool cannot reach:
   - the limit on the output, which that tool does not have (a module whose symbols all share one long name)
   - bad calls
   - SHA-256 against published answers, at many chunk sizes
   - the UTF-8 check against Python's decoder: stdin holds lines of "<hex> <0 or 1>" from tests/utf8_cases.py
   Exit status 0 when all hold. Includes tables.c for its static valid_utf8. */
#include "../tables.c"
#include <stdio.h>

static int bad;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL %s\n", what); bad++; } }

static void test_utf8(void) {
    char line[64];
    unsigned long n = 0, wrong = 0;
    while (fgets(line, sizeof line, stdin)) {
        uint8_t b[8];
        unsigned len = 0, v;
        const char *p = line;
        while (*p != ' ' && sscanf(p, "%2x", &v) == 1 && len < sizeof b) { b[len++] = (uint8_t)v; p += 2; }
        int want = p[1] == '1';
        if (valid_utf8(b, len) != want) { if (wrong++ < 8) printf("UTF-8 differs: %s", line); }
        n++;
    }
    printf("UTF-8: %lu sequences, %lu differ from Python\n", n, wrong);
    check(n > 1000 && !wrong, "UTF-8");
}

static void test_sha(void) {
    static const struct { const char *text, *hex; } v[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}};
    char hex[65];
    for (size_t i = 0; i < sizeof v / sizeof *v; i++) { tokt_sha256_hex(v[i].text, strlen(v[i].text), hex); check(!strcmp(hex, v[i].hex), "SHA-256 vector"); }
    size_t n = 1000000;
    uint8_t *a = malloc(n);
    if (!a) { check(0, "memory"); return; }
    memset(a, 'a', n);
    tokt_sha256_hex(a, n, hex);
    check(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "SHA-256 of a million a");
    /* the same in pieces of every small size */
    for (size_t chunk = 1; chunk < 200; chunk++) {
        tokt_sha256 s;
        uint8_t d[32];
        char h2[65];
        tokt_sha256_init(&s);
        for (size_t at = 0; at < 20000; at += chunk) tokt_sha256_update(&s, a + at, at + chunk > 20000 ? 20000 - at : chunk);
        tokt_sha256_final(&s, d);
        for (int i = 0; i < 32; i++) snprintf(h2 + 2 * i, 3, "%02x", d[i]);
        tokt_sha256_hex(a, 20000, hex);
        check(!strcmp(hex, h2), "SHA-256 in pieces");
    }
    free(a);
}

static void test_calls(void) {
    uint8_t *t = (uint8_t *)1, byte = 0;
    size_t n = 1;
    char err[100];
    tokp_result r;
    memset(&r, 0, sizeof r);
    tokt_module m[TOKT_MODULE_COUNT];
    for (int i = 0; i < TOKT_MODULE_COUNT; i++) m[i] = (tokt_module){&byte, 1, &byte, &r, NULL};
    check(tokt_make(NULL, &t, &n, err, sizeof err) == TOKT_E_ARGS, "no modules");
    check(tokt_make(m, NULL, &n, err, sizeof err) == TOKT_E_ARGS, "no output");
    m[1].rewrite = NULL;
    check(tokt_make(m, &t, &n, err, sizeof err) == TOKT_E_ARGS && !t && !n, "module without a result");
    m[1].rewrite = &r; m[1].image_sha256 = "abc";
    check(tokt_make(m, &t, &n, err, sizeof err) == TOKT_E_ARGS, "a short hash");
    m[1].image_sha256 = "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855";
    check(tokt_make(m, &t, &n, err, sizeof err) == TOKT_E_ARGS, "a hash in capitals");
    m[1].image_sha256 = NULL;
    check(tokt_make(m, &t, &n, err, sizeof err) == TOKT_E_FORMAT && !t, "an image of one byte");
    check(tokt_make(m, &t, &n, NULL, 0) == TOKT_E_FORMAT, "no room for error text");
    check(tokt_check_main(NULL, 0, err, sizeof err) == TOKT_E_ARGS, "no main");
    check(tokt_check_main(&byte, 1, err, sizeof err) == TOKT_E_VERSION, "another main");
}

/* Two million symbols of one 2000-byte name: 4 GB of names from a 64 MB module, stopped at the limit. */
static void test_limit(void) {
    enum { RO = 0x20000, SYMTAB = 0x100000, NAME = 0x10000, DYN = 0x3100000, KEY = 0x3F00000, PATCH = 0x3F10000 };
    uint8_t *img = calloc(PATCH, 1), nso[0x100] = {0};
    if (!img) { check(0, "memory"); return; }
    wr32(img + 4, 0x20);
    memcpy(img + 0x20, "MOD0", 4); wr32(img + 0x24, DYN - 0x20);
    /* DT_HASH chain count */
    wr32(img + RO + 4, 2000000);
    memset(img + NAME, 'x', 2000);
    const uint64_t tags[][2] = {{DT_HASH, RO}, {DT_STRTAB, NAME}, {DT_SYMTAB, SYMTAB}, {DT_RELA, 0}, {DT_RELASZ, 0}, {DT_RELAENT, 24}, {DT_STRSZ, 0x1000}, {DT_SYMENT, 24},
                                {DT_INIT, 0}, {DT_PLTREL, 7}, {DT_JMPREL, 0}, {DT_PLTRELSZ, 0}};
    for (size_t i = 0; i < sizeof tags / sizeof *tags; i++) { wr64(img + DYN + 16 * i, tags[i][0]); wr64(img + DYN + 16 * i + 8, tags[i][1]); }
    tokp_result r;
    memset(&r, 0, sizeof r);
    r.layout = (tokp_layout){.text_end = RO, .ro_off = RO, .data_off = DYN, .key_page_off = KEY, .patch_off = PATCH};
    r.image_size = PATCH;
    tokt_module m[TOKT_MODULE_COUNT];
    for (int i = 0; i < TOKT_MODULE_COUNT; i++) m[i] = (tokt_module){nso, sizeof nso, img, &r, NULL};
    uint8_t *t;
    size_t n;
    char err[100] = "";
    int rc = tokt_make(m, &t, &n, err, sizeof err);
    check(rc == TOKT_E_TOO_BIG, "the size limit");
    if (rc == TOKT_OK) free(t);
    free(img);
}

int main(void) {
    test_sha();
    test_calls();
    test_limit();
    test_utf8();
    printf("%d failed\n", bad);
    return bad != 0;
}

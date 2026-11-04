/* The crypto against published vectors, and the instructions against the portable code at every alignment and length. No game
   files. Run on the device too: `container_cli selftest`.
   Vectors: FIPS-197 C.1, SP 800-38A F.5.5, IEEE 1619 XTS-AES-128 vector 2, FIPS 180 SHA-256. */
#include "selftest.h"
#include "../container_internal.h"
#include <stdio.h>
#include <stdlib.h>

static int hex(const char *s, uint8_t *out) {
    int n = 0;
    for (; s[0] && s[1]; s += 2, n++) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) abort();
        out[n] = (uint8_t)v;
    }
    return n;
}

static int fails;
static void check(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL %s\n", what); fails++; } }

/* CTR the plain way, block by block with a 128-bit counter. */
static void reference_ctr(const tokc_aes *a, const uint8_t iv[16], uint8_t *data, size_t n) {
    uint8_t c[16], ks[16];
    memcpy(c, iv, 16);
    for (size_t off = 0; off < n; off += 16) {
        tokc_aes_encrypt(a, c, ks);
        for (size_t i = 0; i < 16 && off + i < n; i++) data[off + i] ^= ks[i];
        for (int i = 15; i >= 0 && !++c[i]; i--) {}
    }
}

static void block_vectors(int mode, const char *name) {
    char what[80];
    uint8_t key[16], pt[16], ct[16], out[16];
    tokc_aes a;
    hex("000102030405060708090a0b0c0d0e0f", key);
    hex("00112233445566778899aabbccddeeff", pt);
    hex("69c4e0d86a7b0430d8cdb78070b4c55a", ct);
    tokc_aes_init(&a, key, mode);
    tokc_aes_encrypt(&a, pt, out);
    snprintf(what, sizeof what, "%s FIPS-197 encrypt", name); check(!memcmp(out, ct, 16), what);
    tokc_aes_decrypt(&a, ct, out);
    snprintf(what, sizeof what, "%s FIPS-197 decrypt", name); check(!memcmp(out, pt, 16), what);
    uint8_t ctr_key[16], iv[16], plain[64], want[64], data[64];
    hex("2b7e151628aed2a6abf7158809cf4f3c", ctr_key);
    hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv);
    hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", plain);
    hex("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee", want);
    tokc_aes_init(&a, ctr_key, mode);
    memcpy(data, plain, 64);
    reference_ctr(&a, iv, data, 64);
    snprintf(what, sizeof what, "%s SP 800-38A CTR", name); check(!memcmp(data, want, 64), what);

    /* XTS: IEEE 1619 vector 2, then one from OpenSSL with a key whose top bit is set */
    uint8_t k1[16], k2[16], tweak[16], buf[32], c1[32];
    tokc_aes dk, tk;
    static const struct { uint8_t k1, k2; const char *ct; } xts_vectors[] = {
        {0x11, 0x22, "c454185e6a16936e39334038acef838bfb186fff7480adc4289382ecd6d394f0"},
        {0xff, 0x22, "adab41f1753b689b244eeda32b4f6690d30cef54fbf4564f9f80c6e7b35d1a0c"}};
    hex("33333333330000000000000000000000", tweak);
    for (int v = 0; v < 2; v++) {
        memset(k1, xts_vectors[v].k1, 16); memset(k2, xts_vectors[v].k2, 16); memset(buf, 0x44, 32);
        hex(xts_vectors[v].ct, c1);
        tokc_aes_init(&dk, k1, mode); tokc_aes_init(&tk, k2, mode);
        tokc_xts_encrypt(&dk, &tk, tweak, buf, 32);
        snprintf(what, sizeof what, "%s XTS encrypt", name); check(!memcmp(buf, c1, 32), what);
        tokc_xts_decrypt(&dk, &tk, tweak, buf, 32);
        int plain44 = 1;
        for (int i = 0; i < 32; i++) plain44 &= buf[i] == 0x44;
        snprintf(what, sizeof what, "%s XTS decrypt", name); check(plain44, what);
    }
}

static void sha_vectors(void) {
    uint8_t out[32], want[32];
    tokc_sha256_buf("abc", 3, out);
    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want);
    check(!memcmp(out, want, 32), "SHA-256 abc");
    tokc_sha256_buf("", 0, out);
    hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", want);
    check(!memcmp(out, want, 32), "SHA-256 empty");
    tokc_sha256 s;
    tokc_sha256_init(&s);
    uint8_t a[1000];
    memset(a, 'a', sizeof a);
    for (int i = 0; i < 1000; i++) tokc_sha256_update(&s, a, sizeof a);
    tokc_sha256_final(&s, out);
    hex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", want);
    check(!memcmp(out, want, 32), "SHA-256 million a");
}

/* Hardware against portable, and any range decrypted alone against the same range of the whole. */
static void cross_checks(void) {
    enum { N = 4096 };
    uint8_t key[16], high[8], *all = malloc(N), *one = malloc(N), *two = malloc(N);
    uint64_t state = 0x9E3779B97F4A7C15ull;
    tokc_aes hw, sw;
    for (int round = 0; round < 40; round++) {
        for (int i = 0; i < 16; i++) { state = state * 6364136223846793005ull + 1442695040888963407ull; key[i] = (uint8_t)(state >> 33); }
        for (int i = 0; i < 8; i++) { state = state * 6364136223846793005ull + 1442695040888963407ull; high[i] = (uint8_t)(state >> 33); }
        tokc_aes_init(&hw, key, TOKC_AES_AUTO); tokc_aes_init(&sw, key, TOKC_AES_PORTABLE);
        for (int i = 0; i < N; i++) { state = state * 6364136223846793005ull + 1442695040888963407ull; all[i] = (uint8_t)(state >> 33); }
        /* the last rounds put a carry through the counter's bytes in the middle of the buffer */
        uint64_t base = round < 30 ? (state >> 8) & 0xFFFFFFFFF0ull : (((uint64_t)1 << (8 * (1 + (round - 30) % 7))) - 1024) << 4;
        uint8_t iv[16];
        memcpy(iv, high, 8);
        for (int i = 0; i < 8; i++) iv[15 - i] = (uint8_t)((base >> 4) >> 8 * i);
        memcpy(one, all, N); reference_ctr(&sw, iv, one, N);
        memcpy(two, all, N); tokc_aes_ctr(&sw, high, base, two, N);
        check(!memcmp(one, two, N), "CTR, portable against the plain way");
        memcpy(two, all, N); tokc_aes_ctr(&hw, high, base, two, N);
        check(!memcmp(one, two, N), "CTR, instructions against the plain way");
        for (int k = 0; k < 2000; k++) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            size_t off = (size_t)(state >> 40) % N, len = (size_t)(state >> 20 & 0xFFFFF) % (N - off + 1);
            if (k & 1) len = len % 300;
            memcpy(two, all, N);
            tokc_aes_ctr(k & 2 ? &hw : &sw, high, base + off, two + off, len);
            if (memcmp(two + off, one + off, len)) { check(0, "CTR, range on its own"); break; }
        }
        uint8_t in[16], a[16], b[16];
        memcpy(in, all, 16);
        tokc_aes_decrypt(&hw, in, a); tokc_aes_decrypt(&sw, in, b);
        check(!memcmp(a, b, 16), "decrypt, instructions against portable");
    }
    uint8_t nk[32], sector[0x600], enc[0x600];
    memset(nk, 0x5a, sizeof nk);
    for (size_t i = 0; i < sizeof sector; i++) sector[i] = (uint8_t)i;
    memcpy(enc, sector, sizeof enc);
    tokc_nca_xts(nk, enc, sizeof enc, 1);
    check(memcmp(enc, sector, sizeof enc) != 0, "NCA XTS changes the bytes");
    tokc_nca_xts(nk, enc, sizeof enc, 0);
    check(!memcmp(enc, sector, sizeof enc), "NCA XTS round trip");
    free(all); free(one); free(two);
}

int tokc_selftest(void) {
    fails = 0;
    block_vectors(TOKC_AES_PORTABLE, "portable");
    block_vectors(TOKC_AES_AUTO, tokc_aes_impl_name());
    sha_vectors();
    cross_checks();
    printf("%s crypto self-test with %s: %d failures\n", fails ? "FAIL" : "PASS", tokc_aes_impl_name(), fails);
    return fails;
}

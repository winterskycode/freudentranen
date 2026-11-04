/* SHA-256 for the rewritten images (82 MB at each start unless the caller gives their hashes). */
#include "tables.h"
#include <string.h>
#if defined(__aarch64__)
#include <sys/auxv.h>
#ifndef HWCAP_SHA2
#define HWCAP_SHA2 (1 << 6)
#endif
#endif

int tokt_sha256_hw_compiled(void);
void tokt_sha256_hw_blocks(uint32_t h[8], const uint8_t *p, size_t blocks);

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t ror(uint32_t v, unsigned n) { return v >> n | v << (32 - n); }

static void portable_blocks(uint32_t h[8], const uint8_t *p, size_t blocks) {
    for (; blocks; blocks--, p += 64) {
        uint32_t w[64], v[8];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t a = w[i - 15], b = w[i - 2];
            w[i] = w[i - 16] + (ror(a, 7) ^ ror(a, 18) ^ a >> 3) + w[i - 7] + (ror(b, 17) ^ ror(b, 19) ^ b >> 10);
        }
        memcpy(v, h, sizeof v);
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = v[7] + (ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + K[i] + w[i];
            uint32_t t2 = (ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) h[i] += v[i];
    }
}

static int hw_available(void) {
#if defined(__aarch64__)
    return tokt_sha256_hw_compiled() && (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#else
    return 0;
#endif
}

static void blocks(uint32_t h[8], const uint8_t *p, size_t n) {
    if (hw_available()) tokt_sha256_hw_blocks(h, p, n);
    else portable_blocks(h, p, n);
}

const char *tokt_sha_impl_name(void) { return hw_available() ? "armv8" : "portable"; }

void tokt_sha256_init(tokt_sha256 *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->bytes = 0; s->have = 0;
}

void tokt_sha256_update(tokt_sha256 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->bytes += n;
    if (s->have) {
        size_t take = n < 64 - s->have ? n : 64 - s->have;
        memcpy(s->buf + s->have, p, take);
        s->have += take; p += take; n -= take;
        if (s->have < 64) return;
        blocks(s->h, s->buf, 1);
        s->have = 0;
    }
    if (n >= 64) { blocks(s->h, p, n / 64); p += n / 64 * 64; n %= 64; }
    if (n) { memcpy(s->buf, p, n); s->have = n; }
}

void tokt_sha256_final(tokt_sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->bytes * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    tokt_sha256_update(s, &pad, 1);
    while (s->have != 56) tokt_sha256_update(s, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    tokt_sha256_update(s, len, 8);
    for (int i = 0; i < 8; i++) for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(s->h[i] >> (24 - 8 * k));
}

void tokt_sha256_hex(const void *data, size_t n, char hex[65]) {
    static const char digits[] = "0123456789abcdef";
    tokt_sha256 s;
    uint8_t d[32];
    tokt_sha256_init(&s);
    tokt_sha256_update(&s, data, n);
    tokt_sha256_final(&s, d);
    for (int i = 0; i < 32; i++) { hex[2 * i] = digits[d[i] >> 4]; hex[2 * i + 1] = digits[d[i] & 15]; }
    hex[64] = 0;
}

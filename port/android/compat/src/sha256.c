/* SHA-256 behind the CommonCrypto names. The block function uses the ARMv8 SHA-2 instructions (every arm64 Android
   device this port targets has them); the 17 GB RomFS check at first start depends on its speed. */
#include <CommonCrypto/CommonDigest.h>
#include <arm_neon.h>
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

__attribute__((target("sha2")))
static void blocks(uint32_t h[8], const uint8_t *p, size_t n) {
    uint32x4_t s0 = vld1q_u32(h), s1 = vld1q_u32(h + 4);
    for (; n; n--, p += 64) {
        uint32x4_t a = s0, b = s1;
        uint32x4_t m0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p))), m1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16)));
        uint32x4_t m2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 32))), m3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 48)));
        for (int i = 0; i < 16; i++) {   /* m0 always holds the next four schedule words */
            uint32x4_t t = vaddq_u32(m0, vld1q_u32(K + 4 * i)), a0 = a;
            a = vsha256hq_u32(a, b, t);
            b = vsha256h2q_u32(b, a0, t);
            uint32x4_t nw = i < 12 ? vsha256su1q_u32(vsha256su0q_u32(m0, m1), m2, m3) : m0;
            m0 = m1; m1 = m2; m2 = m3; m3 = nw;
        }
        s0 = vaddq_u32(s0, a); s1 = vaddq_u32(s1, b);
    }
    vst1q_u32(h, s0); vst1q_u32(h + 4, s1);
}

int CC_SHA256_Init(CC_SHA256_CTX *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv); c->total = 0; c->used = 0;
    return 1;
}

int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len) {
    const uint8_t *p = data; c->total += len;
    if (c->used) {
        uint32_t take = 64 - c->used < len ? 64 - c->used : len;
        memcpy(c->buf + c->used, p, take); c->used += take; p += take; len -= take;
        if (c->used < 64) return 1;
        blocks(c->h, c->buf, 1); c->used = 0;
    }
    if (len >= 64) { blocks(c->h, p, len / 64); p += len & ~63u; len &= 63; }
    memcpy(c->buf, p, len); c->used = len;
    return 1;
}

int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) {
    uint64_t bits = c->total * 8;
    uint8_t pad[72] = {0x80}; uint32_t n = (c->used < 56 ? 56 : 120) - c->used;
    for (int i = 0; i < 8; i++) pad[n + i] = (uint8_t)(bits >> (56 - 8 * i));
    CC_SHA256_Update(c, pad, n + 8);
    for (int i = 0; i < 8; i++) { md[4 * i] = c->h[i] >> 24; md[4 * i + 1] = c->h[i] >> 16; md[4 * i + 2] = c->h[i] >> 8; md[4 * i + 3] = c->h[i]; }
    return 1;
}

unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md) {
    CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, data, len); CC_SHA256_Final(md, &c);
    return md;
}

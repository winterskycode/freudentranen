// Standard SHA-256 in place of CommonCrypto calls.
#include "legacy_digest.h"
#include <string.h>
#include <stddef.h>
#ifdef __ARM_FEATURE_SHA2
#include <arm_neon.h>
#endif
static void write32(unsigned char *p, uint32_t value, int big) {
    for (unsigned i = 0; i < 4; ++i) p[big ? 3 - i : i] = value >> (8 * i);
}

// Incremental SHA256 is for optional load diagnostics, not validating the user's game packages.
static const uint32_t constants[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#ifdef __ARM_FEATURE_SHA2
/* The CPU's SHA-256 instructions: the pipeline cache file alone is 294 MB to digest at every load and save, 1.3 s of a
   core in plain C */
static void sha256_block(dm_android_sha256_context *c, const unsigned char *data) {
    uint32x4_t s0 = vld1q_u32(c->state), s1 = vld1q_u32(c->state + 4), first = s0, second = s1, m[4];
    for (unsigned i = 0; i < 4; ++i) m[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16 * i)));
    for (unsigned i = 0; i < 16; ++i) {
        uint32x4_t wk = vaddq_u32(m[i & 3], vld1q_u32(constants + 4 * i)), previous = s0;
        s0 = vsha256hq_u32(s0, s1, wk); s1 = vsha256h2q_u32(s1, previous, wk);
        if (i < 12) m[i & 3] = vsha256su1q_u32(vsha256su0q_u32(m[i & 3], m[(i + 1) & 3]), m[(i + 2) & 3], m[(i + 3) & 3]);
    }
    vst1q_u32(c->state, vaddq_u32(s0, first)); vst1q_u32(c->state + 4, vaddq_u32(s1, second));
}
#else
static uint32_t rotate(uint32_t value, unsigned count) { return (value << count) | (value >> (32 - count)); }
static uint32_t read32(const unsigned char *p, int big) {
    if (big) return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
static void sha256_block(dm_android_sha256_context *c, const unsigned char *data) {
    uint32_t w[64]; for (unsigned i = 0; i < 16; ++i) w[i] = read32(data + 4 * i, 1);
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t x = w[i - 15], y = w[i - 2];
        w[i] = w[i - 16] + (rotate(x, 25) ^ rotate(x, 14) ^ (x >> 3)) + w[i - 7] + (rotate(y, 15) ^ rotate(y, 13) ^ (y >> 10));
    }
    uint32_t a=c->state[0], b=c->state[1], d=c->state[3], e=c->state[4], f=c->state[5], g=c->state[6], h=c->state[7], cc=c->state[2];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t t1 = h + (rotate(e,26)^rotate(e,21)^rotate(e,7)) + ((e&f)^(~e&g)) + constants[i] + w[i];
        uint32_t t2 = (rotate(a,30)^rotate(a,19)^rotate(a,10)) + ((a&b)^(a&cc)^(b&cc));
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->state[0]+=a; c->state[1]+=b; c->state[2]+=cc; c->state[3]+=d; c->state[4]+=e; c->state[5]+=f; c->state[6]+=g; c->state[7]+=h;
}
#endif
int dm_android_sha256_init(dm_android_sha256_context *c) {
    *c = (dm_android_sha256_context){.state={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
    return 1;
}
int dm_android_sha256_update(dm_android_sha256_context *c, const void *input, uint32_t length) {
    const unsigned char *data = input; c->length += length;
    while (length) {
        if (!c->used && length >= 64) { sha256_block(c, data); data += 64; length -= 64; continue; }
        unsigned n = 64 - c->used; if (n > length) n = length;
        memcpy(c->buffer + c->used, data, n); c->used += n; data += n; length -= n;
        if (c->used == 64) { sha256_block(c, c->buffer); c->used = 0; }
    }
    return 1;
}
int dm_android_sha256_final(unsigned char output[32], dm_android_sha256_context *c) {
    uint64_t bits = c->length * 8;
    c->buffer[c->used++] = 0x80;
    if (c->used > 56) { memset(c->buffer + c->used, 0, 64 - c->used); sha256_block(c, c->buffer); c->used = 0; }
    memset(c->buffer + c->used, 0, 56 - c->used);
    for (unsigned i = 0; i < 8; ++i) c->buffer[63 - i] = bits >> (8 * i);
    sha256_block(c, c->buffer);
    for (unsigned i = 0; i < 8; ++i) write32(output + 4 * i, c->state[i], 1);
    return 1;
}

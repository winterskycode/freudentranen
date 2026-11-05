/* The SHA-256 instructions of ARMv8. sha256.c picks them when the CPU has them. Keep this file free of libc calls:
   tests/sha_arm64_check.py runs the arm64 object in an emulator. */
#include <stddef.h>
#include <stdint.h>

int tokt_sha256_hw_compiled(void);
void tokt_sha256_hw_blocks(uint32_t h[8], const uint8_t *p, size_t blocks);

#if defined(__aarch64__) && !defined(TOKT_NO_HW)
#include <arm_neon.h>
#define HW __attribute__((target("sha2")))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

int tokt_sha256_hw_compiled(void) { return 1; }

HW void tokt_sha256_hw_blocks(uint32_t h[8], const uint8_t *p, size_t blocks) {
    uint32x4_t s0 = vld1q_u32(h), s1 = vld1q_u32(h + 4);
    for (; blocks; blocks--, p += 64) {
        uint32x4_t a = s0, b = s1, m[4];
        for (int i = 0; i < 4; i++) m[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16 * i)));
#pragma clang loop unroll(full)
        /* four rounds each */
        for (int i = 0; i < 16; i++) {
            uint32x4_t w = vaddq_u32(m[i & 3], vld1q_u32(K + 4 * i)), was = a;
            a = vsha256hq_u32(a, b, w);
            b = vsha256h2q_u32(b, was, w);
            if (i < 12) m[i & 3] = vsha256su1q_u32(vsha256su0q_u32(m[i & 3], m[(i + 1) & 3]), m[(i + 2) & 3], m[(i + 3) & 3]);
        }
        s0 = vaddq_u32(s0, a); s1 = vaddq_u32(s1, b);
    }
    vst1q_u32(h, s0); vst1q_u32(h + 4, s1);
}
#else
int tokt_sha256_hw_compiled(void) { return 0; }
void tokt_sha256_hw_blocks(uint32_t h[8], const uint8_t *p, size_t blocks) { (void)h; (void)p; (void)blocks; }
#endif

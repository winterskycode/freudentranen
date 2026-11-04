/* The AES instructions: ARMv8 Cryptography Extensions on arm64, AES-NI on x86-64. aes.c picks them when the CPU has them.
   CTR runs eight blocks at once to hide the instructions' latency. Keep this file free of libc calls: tests/aes_arm64_check.py
   runs the arm64 object in an emulator. */
#include "container_internal.h"

#define LANES 8

static inline uint64_t swap64(uint64_t v) { return __builtin_bswap64(v); }

#if defined(TOKC_NO_HW)
#define NO_HW 1
#elif defined(__aarch64__)
#include <arm_neon.h>
#define HW __attribute__((target("aes")))

int tokc_aes_hw_compiled(void) { return 1; }

HW void tokc_aes_hw_make_dk(uint8_t dk[176], const uint8_t rk[176]) {
    vst1q_u8(dk, vld1q_u8(rk + 160));
    for (int r = 1; r < 10; r++) vst1q_u8(dk + 16 * r, vaesimcq_u8(vld1q_u8(rk + 160 - 16 * r)));
    vst1q_u8(dk + 160, vld1q_u8(rk));
}

HW static inline uint8x16_t enc(uint8x16_t b, const uint8x16_t k[11]) {
    for (int r = 0; r < 9; r++) b = vaesmcq_u8(vaeseq_u8(b, k[r]));
    return veorq_u8(vaeseq_u8(b, k[9]), k[10]);
}
HW void tokc_aes_hw_encrypt(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8x16_t k[11];
    for (int r = 0; r < 11; r++) k[r] = vld1q_u8(rk + 16 * r);
    vst1q_u8(out, enc(vld1q_u8(in), k));
}
HW void tokc_aes_hw_decrypt(const uint8_t dk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8x16_t b = vld1q_u8(in);
    for (int r = 0; r < 9; r++) b = vaesimcq_u8(vaesdq_u8(b, vld1q_u8(dk + 16 * r)));
    vst1q_u8(out, veorq_u8(vaesdq_u8(b, vld1q_u8(dk + 144)), vld1q_u8(dk + 160)));
}

HW void tokc_aes_hw_ctr(const uint8_t rk[176], const uint8_t high[8], uint64_t block, uint8_t *data, size_t blocks) {
    uint8x16_t k[11];
    for (int r = 0; r < 11; r++) k[r] = vld1q_u8(rk + 16 * r);
    uint8x8_t hi = vld1_u8(high);
    while (blocks >= LANES) {
        uint8x16_t b[LANES];
        for (int i = 0; i < LANES; i++) b[i] = vcombine_u8(hi, vcreate_u8(swap64(block + (uint64_t)i)));
        for (int r = 0; r < 9; r++)
            for (int i = 0; i < LANES; i++) b[i] = vaesmcq_u8(vaeseq_u8(b[i], k[r]));
        for (int i = 0; i < LANES; i++) {
            b[i] = veorq_u8(vaeseq_u8(b[i], k[9]), k[10]);
            vst1q_u8(data + 16 * i, veorq_u8(b[i], vld1q_u8(data + 16 * i)));
        }
        data += 16 * LANES; block += LANES; blocks -= LANES;
    }
    for (; blocks; blocks--, data += 16, block++)
        vst1q_u8(data, veorq_u8(enc(vcombine_u8(hi, vcreate_u8(swap64(block))), k), vld1q_u8(data)));
}

#elif defined(__x86_64__)
#include <immintrin.h>
#define HW __attribute__((target("aes")))

int tokc_aes_hw_compiled(void) { return 1; }

static inline __m128i ld(const uint8_t *p) { return _mm_loadu_si128((const __m128i *)(const void *)p); }
static inline void st(uint8_t *p, __m128i v) { _mm_storeu_si128((__m128i *)(void *)p, v); }

HW void tokc_aes_hw_make_dk(uint8_t dk[176], const uint8_t rk[176]) {
    st(dk, ld(rk + 160));
    for (int r = 1; r < 10; r++) st(dk + 16 * r, _mm_aesimc_si128(ld(rk + 160 - 16 * r)));
    st(dk + 160, ld(rk));
}

HW static inline __m128i enc(__m128i b, const __m128i k[11]) {
    b = _mm_xor_si128(b, k[0]);
    for (int r = 1; r < 10; r++) b = _mm_aesenc_si128(b, k[r]);
    return _mm_aesenclast_si128(b, k[10]);
}
HW void tokc_aes_hw_encrypt(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    __m128i k[11];
    for (int r = 0; r < 11; r++) k[r] = ld(rk + 16 * r);
    st(out, enc(ld(in), k));
}
HW void tokc_aes_hw_decrypt(const uint8_t dk[176], const uint8_t in[16], uint8_t out[16]) {
    __m128i b = _mm_xor_si128(ld(in), ld(dk));
    for (int r = 1; r < 10; r++) b = _mm_aesdec_si128(b, ld(dk + 16 * r));
    st(out, _mm_aesdeclast_si128(b, ld(dk + 160)));
}

HW void tokc_aes_hw_ctr(const uint8_t rk[176], const uint8_t high[8], uint64_t block, uint8_t *data, size_t blocks) {
    __m128i k[11];
    for (int r = 0; r < 11; r++) k[r] = ld(rk + 16 * r);
    uint64_t hi;
    memcpy(&hi, high, 8);
    while (blocks >= LANES) {
        __m128i b[LANES];
        for (int i = 0; i < LANES; i++) b[i] = _mm_xor_si128(_mm_set_epi64x((long long)swap64(block + (uint64_t)i), (long long)hi), k[0]);
        for (int r = 1; r < 10; r++)
            for (int i = 0; i < LANES; i++) b[i] = _mm_aesenc_si128(b[i], k[r]);
        for (int i = 0; i < LANES; i++) {
            b[i] = _mm_aesenclast_si128(b[i], k[10]);
            st(data + 16 * i, _mm_xor_si128(b[i], ld(data + 16 * i)));
        }
        data += 16 * LANES; block += LANES; blocks -= LANES;
    }
    for (; blocks; blocks--, data += 16, block++)
        st(data, _mm_xor_si128(enc(_mm_set_epi64x((long long)swap64(block), (long long)hi), k), ld(data)));
}

#else
#define NO_HW 1
#endif

#ifdef NO_HW
int tokc_aes_hw_compiled(void) { return 0; }
void tokc_aes_hw_make_dk(uint8_t dk[176], const uint8_t rk[176]) { (void)dk; (void)rk; }
void tokc_aes_hw_encrypt(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) { (void)rk; (void)in; (void)out; }
void tokc_aes_hw_decrypt(const uint8_t dk[176], const uint8_t in[16], uint8_t out[16]) { (void)dk; (void)in; (void)out; }
void tokc_aes_hw_ctr(const uint8_t rk[176], const uint8_t high[8], uint64_t block, uint8_t *data, size_t blocks) {
    (void)rk; (void)high; (void)block; (void)data; (void)blocks;
}
#endif

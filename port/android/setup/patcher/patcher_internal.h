/* What the parts of the rewriter share. */
#ifndef TOK_PATCHER_INTERNAL_H
#define TOK_PATCHER_INTERNAL_H
#include "patcher.h"

static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* Writes the message (when err is not NULL) and returns code. */
int tokp_fail(char *err, size_t errlen, int code, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/* d was accepted by tokp_read_layout. Fills img[0, patch_off), decompressing the segments and clearing everything else. */
int tokp_load_image(const uint8_t *d, const tokp_layout *l, uint8_t *img, char *err, size_t errlen);

/* Sorted, unique function starts below text_end from the .eh_frame_hdr the MOD0 header names. len covers the image and
   its key page, at least 0x4000 bytes. *starts is malloc'd. */
int tokp_function_starts(const uint8_t *img, uint64_t len, uint64_t text_end, uint64_t **starts, size_t *count, char *err, size_t errlen);
#endif

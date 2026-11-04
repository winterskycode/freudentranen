/* NSO0 -> memory image, and the function starts from .eh_frame_hdr: the first part of patch_nso.py, bounds-checked. */
#include "patcher_internal.h"
#include <stdlib.h>
#include <string.h>

/* LZ4 block format. The block must produce exactly dn bytes. Short copies run in whole 16 or 8 byte steps where the
   buffers have room for the overrun; every other copy is exact. */
static int lz4_block(const uint8_t *s, size_t sn, uint8_t *d, size_t dn) {
    const uint8_t *ip = s, *iend = s + sn;
    uint8_t *op = d, *oend = d + dn;
    if (!sn) return dn ? -1 : 0;
    for (;;) {
        if (ip >= iend) return -1;
        unsigned token = *ip++;
        size_t lit = token >> 4, more;
        if (lit == 15) do { if (ip >= iend) return -1; more = *ip++; lit += more; } while (more == 255);
        if (lit > (size_t)(iend - ip) || lit > (size_t)(oend - op)) return -1;
        if (lit <= 16 && iend - ip >= 16 && oend - op >= 16) memcpy(op, ip, 16);
        else memcpy(op, ip, lit);
        ip += lit; op += lit;
        if (ip == iend) break;                                  /* the last sequence has literals only */
        if (iend - ip < 2) return -1;
        size_t off = ip[0] | (size_t)ip[1] << 8, len = token & 15;
        ip += 2;
        if (!off || off > (size_t)(op - d)) return -1;
        if (len == 15) do { if (ip >= iend) return -1; more = *ip++; len += more; } while (more == 255);
        len += 4;
        if (len > (size_t)(oend - op)) return -1;
        const uint8_t *m = op - off;
        size_t room = (size_t)(oend - op);
        if (off >= 16 && room >= len + 16) for (size_t k = 0; k < len; k += 16) memcpy(op + k, m + k, 16);
        else if (off >= 8 && room >= len + 8) for (size_t k = 0; k < len; k += 8) memcpy(op + k, m + k, 8);
        else for (size_t k = 0; k < len; k++) op[k] = m[k];     /* overlapping: repeats the pattern */
        op += len;
    }
    return op == oend ? 0 : -1;
}

/* Segments in header order: file offset, memory offset, size, stored size. */
typedef struct { uint32_t file, mem, size, stored; } Seg;

static void read_segments(const uint8_t *d, Seg s[3]) {
    for (int i = 0; i < 3; i++) {
        s[i].file = rd32(d + 0x10 + 0x10 * i); s[i].mem = rd32(d + 0x14 + 0x10 * i); s[i].size = rd32(d + 0x18 + 0x10 * i);
        s[i].stored = rd32(d + 0x60 + 4 * i);
    }
}

int tokp_read_layout(const void *nso, size_t nso_size, tokp_layout *l, char *err, size_t errlen) {
    const uint8_t *d = nso;
    Seg s[3];
    if (!nso || !l) return tokp_fail(err, errlen, TOKP_E_ARGS, "no module");
    memset(l, 0, sizeof *l);
    if (nso_size < 0x100 || memcmp(d, "NSO0", 4)) return tokp_fail(err, errlen, TOKP_E_FORMAT, "not an NSO0 module");
    uint32_t flags = rd32(d + 0xc), bss = rd32(d + 0x3c);
    read_segments(d, s);
    uint64_t end = (uint64_t)s[2].mem + s[2].size + bss, at = 0;
    if (end > TOKP_MAX_IMAGE) return tokp_fail(err, errlen, TOKP_E_FORMAT, "image too large: %#llx", (unsigned long long)end);
    for (int i = 0; i < 3; i++) {
        /* In memory order, inside the image, and stored whole. The Python tool would resize its image on some of these. */
        if (s[i].mem < at || (uint64_t)s[i].mem + s[i].size > end) return tokp_fail(err, errlen, TOKP_E_FORMAT, "segment %d outside the image", i);
        if ((uint64_t)s[i].file + s[i].stored > nso_size) return tokp_fail(err, errlen, TOKP_E_FORMAT, "segment %d outside the module", i);
        if (!(flags >> i & 1) && s[i].stored != s[i].size) return tokp_fail(err, errlen, TOKP_E_FORMAT, "segment %d: stored size differs", i);
        at = (uint64_t)s[i].mem + s[i].size;
    }
    l->text_end = s[0].size; l->ro_off = s[1].mem; l->data_off = s[2].mem;
    l->key_page_off = (end + 0x3FFF) & ~(uint64_t)0x3FFF;
    /* The key page is added to the end of the image, wherever that is; patch_off follows from that length. */
    l->patch_off = (end + TOKP_KEY_PAGE_SIZE + 0xFFFF) & ~(uint64_t)0xFFFF;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { l->build_id[2 * i] = hex[d[0x40 + i] >> 4]; l->build_id[2 * i + 1] = hex[d[0x40 + i] & 15]; }
    return TOKP_OK;
}

int tokp_load_image(const uint8_t *d, const tokp_layout *l, uint8_t *img, char *err, size_t errlen) {
    Seg s[3];
    uint32_t flags = rd32(d + 0xc);
    uint64_t at = 0;
    read_segments(d, s);
    for (int i = 0; i < 3; i++) {
        memset(img + at, 0, s[i].mem - at);
        if (flags >> i & 1) {
            if (lz4_block(d + s[i].file, s[i].stored, img + s[i].mem, s[i].size)) return tokp_fail(err, errlen, TOKP_E_FORMAT, "segment %d does not decompress", i);
        } else memcpy(img + s[i].mem, d + s[i].file, s[i].size);
        at = (uint64_t)s[i].mem + s[i].size;
    }
    memset(img + at, 0, l->patch_off - at);     /* bss, key page, padding */
    return TOKP_OK;
}

/* One value of .eh_frame_hdr. Unsupported formats are errors, as they are in the Python tool (KeyError). */
static int eh_read(const uint8_t *img, uint64_t len, int64_t o, int64_t ehs, unsigned enc, int64_t *v, int *size) {
    unsigned fmt = enc & 0xF, app = enc & 0x70;
    int sz = fmt == 4 ? 8 : 4;
    if ((fmt != 3 && fmt != 0xB && fmt != 4) || o < 0 || (uint64_t)o > len - (uint64_t)sz) return -1;
    int64_t x;
    if (fmt == 3) x = rd32(img + o);
    else if (fmt == 0xB) x = (int32_t)rd32(img + o);
    else { uint64_t u = rd64(img + o); x = u > (uint64_t)1 << 62 ? (int64_t)1 << 62 : (int64_t)u; }   /* huge is all that matters */
    if (app == 0x10) x += o;
    if (app == 0x30) x += ehs;
    *v = x; *size = sz;
    return 0;
}

static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

int tokp_function_starts(const uint8_t *img, uint64_t len, uint64_t text_end, uint64_t **starts, size_t *count, char *err, size_t errlen) {
    *starts = NULL; *count = 0;
    uint64_t m0 = rd32(img + 4);
    if (m0 > len - 20 || memcmp(img + m0, "MOD0", 4)) return tokp_fail(err, errlen, TOKP_E_FORMAT, "no MOD0 header");
    int64_t ehs = (int64_t)m0 + (int32_t)rd32(img + m0 + 16), o, v, nfde;
    int sz;
    if (ehs < 0 || (uint64_t)ehs > len - 4) return tokp_fail(err, errlen, TOKP_E_FORMAT, "eh_frame_hdr outside the image");
    unsigned penc = img[ehs + 1], cenc = img[ehs + 2], tenc = img[ehs + 3];
    o = ehs + 4;
    if (eh_read(img, len, o, ehs, penc, &v, &sz)) goto bad;
    o += sz;
    if (eh_read(img, len, o, ehs, cenc, &nfde, &sz)) goto bad;
    o += sz;
    int tsz = (tenc & 0xF) == 4 ? 8 : 4;
    if (nfde < 0 || (uint64_t)nfde > (len - (uint64_t)o) / (2 * (uint64_t)tsz)) goto bad;
    uint64_t *a = malloc(((size_t)nfde + 1) * sizeof *a);
    size_t n = 0;
    if (!a) return tokp_fail(err, errlen, TOKP_E_NOMEM, "out of memory");
    for (int64_t i = 0; i < nfde; i++) {
        int64_t start, fde;
        if (eh_read(img, len, o, ehs, tenc, &start, &sz)) { free(a); goto bad; }
        o += sz;
        if (eh_read(img, len, o, ehs, tenc, &fde, &sz)) { free(a); goto bad; }
        o += sz;
        if (start < 0) { free(a); goto bad; }
        if ((uint64_t)start < text_end) a[n++] = (uint64_t)start;
    }
    qsort(a, n, sizeof *a, cmp_u64);
    size_t u = 0;
    for (size_t i = 0; i < n; i++) if (!u || a[u - 1] != a[i]) a[u++] = a[i];
    *starts = a; *count = u;
    return TOKP_OK;
bad:
    return tokp_fail(err, errlen, TOKP_E_FORMAT, "bad eh_frame_hdr");
}

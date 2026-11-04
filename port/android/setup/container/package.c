/* The outside of a game file: PFS0 (NSP) and HFS0 (XCI) tables, then what each entry is by its bytes: NCA headers, common
   tickets, and the content meta that says which game and version this is. Entry names are not used, except the XCI's
   "secure" partition, a name in the format itself, and an ExeFS's module names. */
#include "container_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_ENTRIES 4096u
#define MAX_STRINGS (1u << 20)
#define MAX_CNMT (1u << 20)

int tokc_fail(char *err, size_t errlen, int code, const char *fmt, ...) {
    if (err && errlen) {
        va_list a;
        va_start(a, fmt);
        vsnprintf(err, errlen, fmt, a);
        va_end(a);
    }
    return code;
}

void tokc_wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

int tokc_pread_full(int fd, void *buf, size_t n, uint64_t off) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return TOKC_E_IO;
        p += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return TOKC_OK;
}

static int file_read(const void *ctx, void *buf, size_t n, uint64_t off) { return tokc_pread_full(*(const int *)ctx, buf, n, off); }

int tokc_part_parse(tokc_reader read, const void *ctx, uint64_t off, uint64_t bound, const char *magic, uint32_t stride,
                    tokc_entry **out, uint32_t *count, char *err, size_t errlen) {
    uint8_t h[16];
    *out = NULL; *count = 0;
    if (bound < 16) return tokc_fail(err, errlen, TOKC_E_FORMAT, "%s table is cut short", magic);
    int rc = read(ctx, h, 16, off);
    if (rc) return tokc_fail(err, errlen, rc, "cannot read the %s table", magic);
    if (memcmp(h, magic, 4)) return tokc_fail(err, errlen, TOKC_E_FORMAT, "no %s table where there should be one", magic);
    uint32_t n = rd32(h + 4), strsize = rd32(h + 8);
    if (n > MAX_ENTRIES || strsize > MAX_STRINGS) return tokc_fail(err, errlen, TOKC_E_FORMAT, "%s table is too large", magic);
    uint64_t size = 16 + (uint64_t)n * stride + strsize;
    if (size > bound) return tokc_fail(err, errlen, TOKC_E_FORMAT, "%s table runs past its end", magic);
    uint8_t *raw = malloc((size_t)size);
    tokc_entry *e = calloc(n ? n : 1, sizeof *e);
    if (!raw || !e) { free(raw); free(e); return tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory"); }
    if ((rc = read(ctx, raw, (size_t)size, off))) { rc = tokc_fail(err, errlen, rc, "cannot read the %s table", magic); goto fail; }
    const uint8_t *strings = raw + 16 + (size_t)n * stride;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *r = raw + 16 + (size_t)i * stride;
        uint64_t eo = rd64(r), es = rd64(r + 8);
        uint32_t no = rd32(r + 16);
        if (no >= strsize) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "%s entry name is out of bounds", magic); goto fail; }
        const uint8_t *nul = memchr(strings + no, 0, strsize - no);
        if (!nul) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "%s entry name is not terminated", magic); goto fail; }
        size_t len = (size_t)(nul - (strings + no));
        if (len >= sizeof e[i].name) len = sizeof e[i].name - 1;
        memcpy(e[i].name, strings + no, len);
        if (!fits(size, eo, bound) || !fits(size + eo, es, bound)) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "%s entry runs past the end of the file", magic); goto fail; }
        e[i].off = off + size + eo; e[i].size = es;
    }
    free(raw);
    *out = e; *count = n;
    return TOKC_OK;
fail:
    free(raw); free(e);
    return rc;
}

/* A ticket is told by its issuer and format, not its name. */
static int read_ticket(tokc_package *p, const tokc_entry *e, char *err, size_t errlen) {
    uint8_t sig[4], t[0x180];
    if (e->size < 0x80 + sizeof t) return TOKC_OK;
    int rc = tokc_pread_full(p->fd, sig, 4, e->off);
    if (rc) return tokc_fail(err, errlen, rc, "cannot read the game file");
    uint32_t body;
    switch (rd32(sig)) {
    case 0x10000: case 0x10003: body = 0x240; break;
    case 0x10001: case 0x10004: body = 0x140; break;
    case 0x10002: case 0x10005: body = 0x80; break;
    default: return TOKC_OK;
    }
    if (e->size < body + sizeof t) return TOKC_OK;
    if ((rc = tokc_pread_full(p->fd, t, sizeof t, e->off + body))) return tokc_fail(err, errlen, rc, "cannot read the game file");
    if (memcmp(t, "Root-", 5) || !memchr(t, 0, 0x40) || !strstr((const char *)t, "-XS") || t[0x140] != 2) return TOKC_OK;
    for (uint32_t i = 0; i < p->ntickets; i++) if (!memcmp(p->tickets[i].rights, t + 0x160, 16)) return TOKC_OK;
    tokc_ticket *n = realloc(p->tickets, (p->ntickets + 1) * sizeof *n);
    if (!n) return tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory");
    p->tickets = n; n += p->ntickets++;
    memcpy(n->rights, t + 0x160, 16);
    memcpy(n->key, t + 0x40, 16);
    n->personalized = t[0x141] != 0;
    tokc_wipe(t, sizeof t);
    return TOKC_OK;
}

/* Adds the entry if its first bytes decrypt to an NCA header; anything else is left for read_ticket. */
static int read_nca(tokc_package *p, const tokc_entry *e, const tokc_keys *keys, int *is_nca, char *err, size_t errlen) {
    uint8_t h[TOKC_NCA_HEADER];
    *is_nca = 0;
    if (e->size < sizeof h) return TOKC_OK;
    int rc = tokc_pread_full(p->fd, h, sizeof h, e->off);
    if (rc) return tokc_fail(err, errlen, rc, "cannot read the game file");
    tokc_nca_xts(keys->header, h, sizeof h, 0);
    if (!memcmp(h + 0x200, "NCA2", 4) || !memcmp(h + 0x200, "NCA1", 4) || !memcmp(h + 0x200, "NCA0", 4)) return tokc_fail(err, errlen, TOKC_E_FORMAT, "this game file uses an old NCA format");
    if (memcmp(h + 0x200, "NCA3", 4)) return TOKC_OK;
    *is_nca = 1;
    uint64_t size = rd64(h + 0x208);
    if (size != e->size) {
        char m[8];
        if (e->size > 0x4008 && !tokc_pread_full(p->fd, m, 8, e->off + 0x4000) && !memcmp(m, "NCZSECTN", 8))
            return tokc_fail(err, errlen, TOKC_E_FORMAT, "this game file is compressed; only plain NSP and XCI files can be read in place");
        return tokc_fail(err, errlen, TOKC_E_FORMAT, "an NCA's size does not match its place in the file: the file is damaged");
    }
    for (int i = 0; i < 4; i++) {
        uint64_t start = rd32(h + 0x240 + 16 * i), end = rd32(h + 0x244 + 16 * i);
        if (!start && !end) continue;
        if (start < TOKC_NCA_HEADER / 0x200 || start >= end || end * 0x200 > size)
            return tokc_fail(err, errlen, TOKC_E_FORMAT, "an NCA section is out of bounds");
    }
    tokc_nca *n = realloc(p->ncas, (p->nncas + 1) * sizeof *n);
    if (!n) return tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory");
    p->ncas = n; n += p->nncas++;
    memset(n, 0, sizeof *n);
    memcpy(n->hdr, h, sizeof h);
    n->fd = p->fd; n->off = e->off; n->size = size;
    n->content_type = h[0x205]; n->area_index = h[0x207];
    unsigned g = h[0x206] > h[0x220] ? h[0x206] : h[0x220];
    n->generation = g ? g - 1 : 0;
    n->title_id = rd64(h + 0x210);
    static const uint8_t zero[16];
    n->has_rights = memcmp(h + 0x230, zero, 16) != 0;
    return TOKC_OK;
}

int tokc_section_present(const tokc_nca *n, int i) { return rd32(n->hdr + 0x240 + 16 * i) || rd32(n->hdr + 0x244 + 16 * i); }

static int section_key(uint8_t key[16], const tokc_nca *n, const tokc_keys *keys, const tokc_package *const *packages, int npackages, char *err, size_t errlen) {
    unsigned g = n->generation;
    if (g >= TOKC_GENERATIONS) return tokc_fail(err, errlen, TOKC_E_FORMAT, "an NCA names key generation %u, which cannot be", g);
    if (n->has_rights) {
        const tokc_ticket *t = NULL;
        for (int i = 0; i < npackages && !t; i++)
            for (uint32_t k = 0; k < packages[i]->ntickets && !t; k++)
                if (!memcmp(packages[i]->tickets[k].rights, n->hdr + 0x230, 16)) t = &packages[i]->tickets[k];
        if (!t) return tokc_fail(err, errlen, TOKC_E_TICKET, "the file has no ticket for its content");
        if (t->personalized) return tokc_fail(err, errlen, TOKC_E_TICKET, "the file's ticket is tied to one console; a common ticket is needed");
        if (!(keys->have_titlekek >> g & 1)) return tokc_fail(err, errlen, TOKC_E_KEY_MISSING, "prod.keys has no titlekek_%02x", g);
        tokc_aes_ecb_decrypt(keys->titlekek[g], t->key, key, 16);
        return TOKC_OK;
    }
    if (n->area_index) return tokc_fail(err, errlen, TOKC_E_FORMAT, "an NCA uses key area %u; only the application's is supported", n->area_index);
    if (!(keys->have_area_app >> g & 1)) return tokc_fail(err, errlen, TOKC_E_KEY_MISSING, "prod.keys has no key_area_key_application_%02x", g);
    tokc_aes_ecb_decrypt(keys->area_app[g], n->hdr + 0x300 + 32, key, 16);   /* the third slot is the CTR key */
    return TOKC_OK;
}

int tokc_section_open(tokc_section *s, const tokc_nca *n, int i, const tokc_keys *keys, const tokc_package *const *packages, int npackages, char *err, size_t errlen) {
    const uint8_t *fs = NCA_FS(n, i);
    uint8_t hash[32], key[16];
    memset(s, 0, sizeof *s);
    s->fd = -1;
    if (!tokc_section_present(n, i)) return tokc_fail(err, errlen, TOKC_E_FORMAT, "an NCA section is missing");
    tokc_sha256_buf(fs, 0x200, hash);
    if (memcmp(hash, n->hdr + 0x280 + 32 * i, 32)) return tokc_fail(err, errlen, TOKC_E_FORMAT, "an NCA section header fails its hash: the file is damaged");
    int crypt = fs[4];
    if (crypt != CRYPT_NONE && crypt != CRYPT_CTR && crypt != CRYPT_CTR_EX)
        return tokc_fail(err, errlen, TOKC_E_FORMAT, "an NCA section uses encryption type %d, which is not supported", crypt);
    /* sparse and compressed sections: bucket tables at 0x148 and 0x178 */
    if (rd64(fs + 0x150) || rd64(fs + 0x180)) return tokc_fail(err, errlen, TOKC_E_FORMAT, "an NCA section is sparse or compressed, which is not supported");
    uint64_t start = (uint64_t)rd32(n->hdr + 0x240 + 16 * i) * 0x200, end = (uint64_t)rd32(n->hdr + 0x244 + 16 * i) * 0x200;
    s->fd = n->fd; s->nca_off = start; s->size = end - start; s->file_off = n->off + start;
    for (int k = 0; k < 8; k++) s->high[k] = fs[0x147 - k];
    s->encrypted = crypt != CRYPT_NONE;
    if (s->encrypted) {
        int rc = section_key(key, n, keys, packages, npackages, err, errlen);
        if (rc) return rc;
        tokc_aes_init(&s->aes, key, TOKC_AES_AUTO);
        tokc_wipe(key, sizeof key);
    }
    return TOKC_OK;
}

int tokc_section_read(const void *section, void *buf, size_t n, uint64_t off) {
    const tokc_section *s = section;
    if (!fits(off, n, s->size)) return TOKC_E_FORMAT;
    int rc = tokc_pread_full(s->fd, buf, n, s->file_off + off);
    if (rc) return rc;
    if (s->encrypted) tokc_aes_ctr(&s->aes, s->high, s->nca_off + off, buf, n);
    return TOKC_OK;
}

/* The content meta in a meta NCA: a PFS0 holding one .cnmt, whatever it is called. */
static int read_meta(tokc_package *p, const tokc_nca *n, char *err, size_t errlen, const tokc_keys *keys) {
    const tokc_package *self = p;
    tokc_section s;
    tokc_entry *e = NULL;
    uint32_t count = 0;
    uint8_t *cnmt = NULL;
    int rc = tokc_section_open(&s, n, 0, keys, &self, 1, err, errlen);
    if (rc) goto out;
    const uint8_t *fs = NCA_FS(n, 0);
    uint64_t pfs = rd64(fs + 0x40), pfs_size = rd64(fs + 0x48);
    if (fs[2] != FS_PFS0 || !fits(pfs, pfs_size, s.size)) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "the content meta section is malformed"); goto out; }
    uint8_t magic[4];
    if ((rc = tokc_section_read(&s, magic, 4, pfs))) { rc = tokc_fail(err, errlen, rc, "cannot read the content meta"); goto out; }
    if (memcmp(magic, "PFS0", 4)) { rc = tokc_fail(err, errlen, TOKC_E_KEY_WRONG, "the keys do not decrypt the content meta: prod.keys does not match this file"); goto out; }
    if ((rc = tokc_part_parse(tokc_section_read, &s, pfs, pfs_size, "PFS0", 24, &e, &count, err, errlen))) goto out;
    if (count != 1 || e[0].size < 0x20 || e[0].size > MAX_CNMT) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "the content meta is malformed"); goto out; }
    cnmt = malloc((size_t)e[0].size);
    if (!cnmt) { rc = tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory"); goto out; }
    if ((rc = tokc_section_read(&s, cnmt, (size_t)e[0].size, e[0].off))) { rc = tokc_fail(err, errlen, rc, "cannot read the content meta"); goto out; }
    tokc_meta *m = realloc(p->metas, (p->nmetas + 1) * sizeof *m);
    if (!m) { rc = tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory"); goto out; }
    p->metas = m; m += p->nmetas++;
    m->title_id = rd64(cnmt); m->version = rd32(cnmt + 8); m->type = cnmt[0xC];
out:
    free(cnmt); free(e);
    tokc_wipe(&s, sizeof s);
    return rc;
}

/* The entries of a file's game content: the root of an NSP, the secure partition of an XCI. */
static int read_root(tokc_package *p, tokc_entry **out, uint32_t *count, char *err, size_t errlen) {
    uint8_t m[4];
    if (p->size < 0x10) return tokc_fail(err, errlen, TOKC_E_FORMAT, "this is not an NSP or XCI game file: it is too small");
    int rc = tokc_pread_full(p->fd, m, 4, 0);
    if (rc) return tokc_fail(err, errlen, rc, "cannot read the game file");
    if (!memcmp(m, "PFS0", 4)) return tokc_part_parse(file_read, &p->fd, 0, p->size, "PFS0", 24, out, count, err, errlen);
    uint8_t h[0x40];
    if (p->size < 0x100 + sizeof h) return tokc_fail(err, errlen, TOKC_E_FORMAT, "this is not an NSP or XCI game file");
    if ((rc = tokc_pread_full(p->fd, h, sizeof h, 0x100))) return tokc_fail(err, errlen, rc, "cannot read the game file");
    if (memcmp(h, "HEAD", 4)) return tokc_fail(err, errlen, TOKC_E_FORMAT, "this is not an NSP or XCI game file");
    uint64_t root = rd64(h + 0x30);
    if (root >= p->size) return tokc_fail(err, errlen, TOKC_E_FORMAT, "the XCI's partition table is out of bounds");
    tokc_entry *parts;
    uint32_t n, found = 0;
    if ((rc = tokc_part_parse(file_read, &p->fd, root, p->size - root, "HFS0", 64, &parts, &n, err, errlen))) return rc;
    tokc_entry secure = {{0}, 0, 0};
    for (uint32_t i = 0; i < n; i++) if (!strcmp(parts[i].name, "secure")) { secure = parts[i]; found++; }
    free(parts);
    if (found != 1) return tokc_fail(err, errlen, TOKC_E_FORMAT, "the XCI has no secure partition");
    p->is_xci = 1;
    return tokc_part_parse(file_read, &p->fd, secure.off, secure.size, "HFS0", 64, out, count, err, errlen);
}

int tokc_package_open(tokc_package *p, int fd, const tokc_keys *keys, char *err, size_t errlen) {
    struct stat st;
    memset(p, 0, sizeof *p);
    p->fd = fd;
    if (!keys) return tokc_fail(err, errlen, TOKC_E_ARGS, "no keys");
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) return tokc_fail(err, errlen, TOKC_E_IO, "the game file cannot be read as a file");
    p->size = (uint64_t)st.st_size;
    tokc_entry *e = NULL;
    uint32_t n = 0;
    int rc = read_root(p, &e, &n, err, errlen);
    if (rc) goto fail;
    for (uint32_t i = 0; i < n && !rc; i++) {
        int is_nca;
        rc = read_nca(p, &e[i], keys, &is_nca, err, errlen);
        if (!rc && !is_nca) rc = read_ticket(p, &e[i], err, errlen);
    }
    free(e); e = NULL;
    if (rc) goto fail;
    if (!p->nncas) { rc = tokc_fail(err, errlen, TOKC_E_KEY_WRONG, "no NCA in this file decrypts: it is not a game file, or header_key in prod.keys is wrong"); goto fail; }
    for (uint32_t i = 0; i < p->nncas; i++)
        if (p->ncas[i].content_type == 1 && (rc = read_meta(p, &p->ncas[i], err, errlen, keys))) goto fail;
    if (!p->nmetas) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "this file has no content meta, so it cannot be identified"); goto fail; }
    return TOKC_OK;
fail:
    free(e);
    tokc_package_free(p);
    return rc;
}

void tokc_package_free(tokc_package *p) {
    if (p->ncas) tokc_wipe(p->ncas, p->nncas * sizeof *p->ncas);
    if (p->tickets) tokc_wipe(p->tickets, p->ntickets * sizeof *p->tickets);
    free(p->ncas); free(p->tickets); free(p->metas);
    memset(p, 0, sizeof *p);
    p->fd = -1;
}

int tokc_inspect(int fd, const tokc_keys *keys, tokc_info *info, char *err, size_t errlen) {
    tokc_package p;
    if (!info) return tokc_fail(err, errlen, TOKC_E_ARGS, "no output");
    memset(info, 0, sizeof *info);
    int rc = tokc_package_open(&p, fd, keys, err, errlen);
    if (rc) return rc;
    const tokc_meta *m = &p.metas[0];
    for (uint32_t i = 0; i < p.nmetas; i++)
        if (p.metas[i].title_id == TOKC_TITLE_BASE || p.metas[i].title_id == TOKC_TITLE_UPDATE) { m = &p.metas[i]; break; }
    info->title_id = m->title_id; info->version = m->version; info->type = m->type;
    info->is_xci = p.is_xci; info->metas = (int)p.nmetas;
    tokc_package_free(&p);
    return TOKC_OK;
}

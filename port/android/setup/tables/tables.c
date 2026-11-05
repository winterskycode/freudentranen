/* prepare_runtime.py in C. The steps keep the script's order and its checks; each assert or exception there is an error
   here. The file: a 64-byte header, one 208-byte record per module, each module's symbols, relocations and gates (24 bytes
   each, so everything stays 8-byte aligned), then the symbol names. */
#include "tables.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER 64u
#define RECORD 208u
/* a symbol, relocation or gate in the file; also an ELF symbol and RELA entry */
#define ENTRY 24u
#define MAX_TAG 37
#define DYNAMIC_MAX 4096u

/* ELF dynamic tags, relocation kinds */
enum { DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8, DT_RELAENT = 9, DT_STRSZ = 10, DT_SYMENT = 11, DT_INIT = 12,
       DT_PLTREL = 20, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27, DT_RELRSZ = 35, DT_RELR = 36, DT_RELRENT = 37 };
enum { R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027 };

/* The slot main's PLT probe looks for. runtime/loader.c probe() names the same symbol. */
static const char probe_symbol[] = "_ZN2nn2os13GetSystemTickEv";

const char *const tokt_module_names[TOKT_MODULE_COUNT] = {"main", "subsdk0", "sdk"};

static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static inline void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static inline void wr64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }

__attribute__((format(printf, 4, 5)))
static int fail(char *err, size_t errlen, int code, const char *fmt, ...) {
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return code;
}

typedef struct { uint8_t *p; size_t n, cap; } Pool;

/* What the steps share: the output, and the module at hand. */
typedef struct {
    /* header, records, arrays; the names follow when all is done */
    Pool out;
    Pool names;
    const char *mod;
    const uint8_t *img;
    uint64_t size;
    const tokp_layout *l;
    /* have: a bit per tag */
    uint64_t tag[MAX_TAG + 1], have;
    uint32_t nsym;
    /* where this module's symbols start in out */
    size_t symbols;
    /* a bit per byte offset from data_off: the addresses relocated so far */
    uint8_t *seen;
    int want_probe, found_probe;
    uint64_t probe_slot;
    char *err; size_t errlen;
} Ctx;

#define BAD(c, ...) fail((c)->err, (c)->errlen, TOKT_E_FORMAT, __VA_ARGS__)

/* Room for add more bytes; both pools count towards the limit. */
static int room(Ctx *c, Pool *b, size_t add) {
    if (add >= TOKT_MAX_TABLES || c->out.n + c->names.n + add >= TOKT_MAX_TABLES)
        return fail(c->err, c->errlen, TOKT_E_TOO_BIG, "%s: the tables would be too large", c->mod);
    if (add <= b->cap - b->n) return TOKT_OK;
    size_t cap = b->cap ? b->cap : 1u << 16;
    while (cap - b->n < add) cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p) return fail(c->err, c->errlen, TOKT_E_NOMEM, "out of memory");
    b->p = p; b->cap = cap;
    return TOKT_OK;
}

static int in_image(const Ctx *c, uint64_t off, uint64_t n) { return off <= c->size && n <= c->size - off; }

static int has(const Ctx *c, unsigned tag) { return c->have >> tag & 1; }

/* The dynamic table, through MOD0. A tag seen twice keeps its last value. */
static int read_tags(Ctx *c) {
    static const unsigned needed[] = {DT_PLTRELSZ, DT_RELA, DT_RELASZ, DT_RELAENT, DT_SYMENT, DT_INIT, DT_PLTREL, DT_JMPREL};
    c->have = 0;
    if (c->size < 8) return BAD(c, "%s: image too short", c->mod);
    uint64_t mod0 = rd32(c->img + 4);
    if (!in_image(c, mod0, 8) || memcmp(c->img + mod0, "MOD0", 4)) return BAD(c, "%s: no MOD0 header", c->mod);
    int64_t dyn = (int64_t)mod0 + (int32_t)rd32(c->img + mod0 + 4);
    if (dyn < 0) return BAD(c, "%s: dynamic table outside the image", c->mod);
    for (unsigned i = 0;; i++) {
        uint64_t at = (uint64_t)dyn + 16 * (uint64_t)i;
        if (i == DYNAMIC_MAX) return BAD(c, "%s: unterminated dynamic table", c->mod);
        if (!in_image(c, at, 16)) return BAD(c, "%s: dynamic table outside the image", c->mod);
        uint64_t tag = rd64(c->img + at);
        if (!tag) break;
        if (tag <= MAX_TAG) { c->tag[tag] = rd64(c->img + at + 8); c->have |= 1ull << tag; }
    }
    for (size_t i = 0; i < sizeof needed / sizeof *needed; i++)
        if (!has(c, needed[i])) return BAD(c, "%s: dynamic tag %u missing", c->mod, needed[i]);
    return TOKT_OK;
}

/* Python's bytes.decode(): well-formed UTF-8, nothing else. */
static int valid_utf8(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t b = s[i];
        size_t more;
        uint8_t lo = 0x80, hi = 0xBF;
        if (b < 0x80) { i++; continue; }
        if (b >= 0xC2 && b <= 0xDF) more = 1;
        else if (b >= 0xE0 && b <= 0xEF) { more = 2; if (b == 0xE0) lo = 0xA0; if (b == 0xED) hi = 0x9F; }
        else if (b >= 0xF0 && b <= 0xF4) { more = 3; if (b == 0xF0) lo = 0x90; if (b == 0xF4) hi = 0x8F; }
        else return 0;
        if (more >= n - i) return 0;
        for (size_t k = 1; k <= more; k++) {
            if (s[i + k] < lo || s[i + k] > hi) return 0;
            lo = 0x80; hi = 0xBF;
        }
        i += more + 1;
    }
    return 1;
}

/* The count of symbols: DT_HASH's chain count, or for a module with GNU hash only the dynsym extent in the NSO header. */
static int symbol_count(Ctx *c, const tokt_module *m) {
    if (has(c, DT_HASH)) {
        if (!in_image(c, c->tag[DT_HASH], 8)) return BAD(c, "%s: DT_HASH outside the image", c->mod);
        c->nsym = rd32(c->img + c->tag[DT_HASH] + 4);
        return TOKT_OK;
    }
    if (m->nso_size < 0xA0 || !has(c, DT_SYMTAB)) return BAD(c, "%s: no dynsym extent", c->mod);
    const uint8_t *nso = m->nso;
    uint64_t symtab = c->tag[DT_SYMTAB], offset = rd32(nso + 0x98), size = rd32(nso + 0x9C);
    if (c->l->ro_off + offset != symtab || size % ENTRY) return BAD(c, "%s: dynsym does not match the NSO header", c->mod);
    if (!size || symtab > c->l->data_off || size > c->l->data_off - symtab) return BAD(c, "%s: dynsym extent", c->mod);
    c->nsym = (uint32_t)(size / ENTRY);
    return TOKT_OK;
}

static int emit_symbols(Ctx *c) {
    c->symbols = c->out.n;
    if (!c->nsym) return TOKT_OK;
    if (!has(c, DT_SYMTAB) || !has(c, DT_STRTAB) || !has(c, DT_STRSZ)) return BAD(c, "%s: symbol tags missing", c->mod);
    uint64_t symtab = c->tag[DT_SYMTAB], strtab = c->tag[DT_STRTAB], strsz = c->tag[DT_STRSZ];
    uint64_t end = strsz > UINT64_MAX - strtab ? UINT64_MAX : strtab + strsz;
    if (end > c->size) end = c->size;
    if (!in_image(c, symtab, (uint64_t)c->nsym * ENTRY)) return BAD(c, "%s: symbols outside the image", c->mod);
    if (strtab > c->size) return BAD(c, "%s: names outside the image", c->mod);
    int rc = room(c, &c->out, (size_t)c->nsym * ENTRY);
    if (rc) return rc;
    for (uint32_t i = 0; i < c->nsym; i++) {
        const uint8_t *s = c->img + symtab + (uint64_t)ENTRY * i;
        uint64_t from = strtab + rd32(s);
        const uint8_t *nul = from < end ? memchr(c->img + from, 0, (size_t)(end - from)) : NULL;
        if (!nul) return BAD(c, "%s: symbol %u has no name", c->mod, i);
        size_t len = (size_t)(nul - (c->img + from));
        if (!valid_utf8(c->img + from, len)) return BAD(c, "%s: symbol %u: name is not UTF-8", c->mod, i);
        if ((rc = room(c, &c->names, len + 1))) return rc;
        uint8_t *o = c->out.p + c->out.n;
        /* the file's symbol: value, size, name, defined (a section index), binding, type */
        wr64(o, rd64(s + 8));
        wr64(o + 8, rd64(s + 16));
        wr32(o + 16, (uint32_t)c->names.n);
        o[20] = s[6] || s[7];
        o[21] = s[4] >> 4; o[22] = s[4] & 15;
        o[23] = 0;
        memcpy(c->names.p + c->names.n, c->img + from, len + 1);
        c->names.n += len + 1;
        c->out.n += ENTRY;
    }
    return TOKT_OK;
}

/* Marks an address as relocated; 1 when it already was. The caller checked it lies in [data_off, key_page_off - 8]. */
static int mark(Ctx *c, uint64_t where) {
    uint64_t bit = where - c->l->data_off;
    uint8_t *b = &c->seen[bit >> 3], m = (uint8_t)(1u << (bit & 7));
    if (*b & m) return 1;
    *b |= m;
    return 0;
}

static int put_reloc(Ctx *c, uint64_t where, uint32_t kind, uint32_t symbol, uint64_t addend) {
    int rc = room(c, &c->out, ENTRY);
    if (rc) return rc;
    uint8_t *o = c->out.p + c->out.n;
    wr64(o, where); wr32(o + 8, kind); wr32(o + 12, symbol); wr64(o + 16, addend);
    c->out.n += ENTRY;
    return TOKT_OK;
}

static int relr_one(Ctx *c, uint64_t where) {
    uint64_t lower = c->l->data_off, upper = c->l->key_page_off;
    if (where % 8 || where < lower || upper < 8 || where > upper - 8 || !in_image(c, where, 8))
        return BAD(c, "%s: RELR relocation outside writable image: %#llx", c->mod, (unsigned long long)where);
    if (mark(c, where)) return BAD(c, "%s: duplicate RELR relocation: %#llx", c->mod, (unsigned long long)where);
    uint64_t addend = rd64(c->img + where);
    if (addend >= upper) return BAD(c, "%s: RELR addend outside module: %#llx", c->mod, (unsigned long long)where);
    return put_reloc(c, where, R_RELATIVE, 0, addend);
}

/* relr.py decode_relr */
static int emit_relr(Ctx *c) {
    int count = has(c, DT_RELRSZ) + has(c, DT_RELR) + has(c, DT_RELRENT);
    if (!count) return TOKT_OK;
    if (count != 3 || c->tag[DT_RELRENT] != 8 || c->tag[DT_RELRSZ] % 8) return BAD(c, "%s: invalid RELR dynamic tags", c->mod);
    uint64_t table = c->tag[DT_RELR], size = c->tag[DT_RELRSZ], cursor = 0;
    if (!in_image(c, table, size)) return BAD(c, "%s: RELR table outside image", c->mod);
    int started = 0, rc;
    for (uint64_t at = table; at < table + size; at += 8) {
        uint64_t entry = rd64(c->img + at);
        if (!(entry & 1)) {
            if ((rc = relr_one(c, entry))) return rc;
            cursor = entry + 8; started = 1;
        } else {
            if (!started) return BAD(c, "%s: RELR bitmap before initial address", c->mod);
            for (unsigned bit = 1; bit < 64; bit++)
                if (entry >> bit & 1 && (rc = relr_one(c, cursor + (bit - 1) * 8))) return rc;
            cursor += 63 * 8;
        }
    }
    return TOKT_OK;
}

static const char *symbol_name(const Ctx *c, uint32_t symbol) {
    return (const char *)c->names.p + rd32(c->out.p + c->symbols + (size_t)symbol * ENTRY + 16);
}

static int emit_rela(Ctx *c, uint64_t address, uint64_t size) {
    uint64_t lower = c->l->data_off, upper = c->l->key_page_off;
    if (size % ENTRY) return BAD(c, "%s: RELA size", c->mod);
    if (size && !in_image(c, address, size)) return BAD(c, "%s: RELA table outside the image", c->mod);
    for (uint64_t at = address; at - address < size; at += ENTRY) {
        const uint8_t *e = c->img + at;
        uint64_t where = rd64(e), info = rd64(e + 8), addend = rd64(e + 16);
        uint64_t kind = info & 0xFFFFFFFF, symbol = info >> 32;
        if ((kind != R_ABS64 && kind != R_GLOB_DAT && kind != R_JUMP_SLOT && kind != R_RELATIVE) || symbol >= c->nsym)
            return BAD(c, "%s: relocation kind or symbol", c->mod);
        if (where < lower || upper < 8 || where > upper - 8) return BAD(c, "%s: relocation outside writable image: %#llx", c->mod, (unsigned long long)where);
        if (kind == R_RELATIVE && addend >= upper) return BAD(c, "%s: relative addend: %#llx", c->mod, (unsigned long long)where);
        if (mark(c, where)) return BAD(c, "%s: duplicate relocation: %#llx", c->mod, (unsigned long long)where);
        int rc = put_reloc(c, where, (uint32_t)kind, (uint32_t)symbol, addend);
        if (rc) return rc;
        if (c->want_probe && !c->found_probe && kind == R_JUMP_SLOT && !strcmp(symbol_name(c, (uint32_t)symbol), probe_symbol)) {
            c->found_probe = 1; c->probe_slot = where;
        }
    }
    return TOKT_OK;
}

static int gate_order(const void *a, const void *b) {
    uint64_t x = ((const tokp_gate *)a)->ret, y = ((const tokp_gate *)b)->ret;
    return (x > y) - (x < y);
}

/* Sorted by return address: loader.c runtime_dispatch looks them up by binary search. timer: 0 svc, 1 a timer read. */
static int emit_gates(Ctx *c, const tokp_result *r) {
    if (r->ngates && !r->gates) return BAD(c, "%s: no gates", c->mod);
    if (r->ngates > TOKT_MAX_TABLES / ENTRY) return fail(c->err, c->errlen, TOKT_E_TOO_BIG, "%s: too many gates", c->mod);
    int rc = room(c, &c->out, r->ngates * ENTRY);
    if (rc) return rc;
    tokp_gate *g = malloc((r->ngates ? r->ngates : 1) * sizeof *g);
    if (!g) return fail(c->err, c->errlen, TOKT_E_NOMEM, "out of memory");
    if (r->ngates) memcpy(g, r->gates, r->ngates * sizeof *g);
    qsort(g, r->ngates, sizeof *g, gate_order);
    for (size_t i = 0; i < r->ngates; i++) {
        uint32_t timer = g[i].kind == TOKP_CNTPCT || g[i].kind == TOKP_CNTVCT;
        if ((!timer && g[i].kind != TOKP_SVC) || (i && g[i].ret == g[i - 1].ret)) {
            unsigned long long ret = g[i].ret;
            free(g);
            return BAD(c, "%s: bad gate at %#llx", c->mod, ret);
        }
        uint8_t *o = c->out.p + c->out.n;
        wr64(o, g[i].ret); wr64(o + 8, g[i].original); wr32(o + 16, timer); wr32(o + 20, g[i].arg);
        c->out.n += ENTRY;
    }
    free(g);
    return TOKT_OK;
}

/* An adrp's page displacement, in pages. */
static int64_t adrp_pages(uint32_t a) {
    int64_t imm = (int64_t)((a >> 29 & 3) | (a >> 5 & 0x7FFFF) << 2);
    return (imm ^ (1 << 20)) - (1 << 20);
}

/* The word DT_INIT tests to skip constructors: adrp x19 then ldr w8, [x19, #n] at this version's offsets. */
static int find_ctor_flag(Ctx *c, uint64_t *flag) {
    enum { REG = 19, ADRP_AT = 16, LOAD_AT = 20 };
    uint64_t init = c->tag[DT_INIT];
    if (!in_image(c, init, LOAD_AT + 4)) return BAD(c, "%s: DT_INIT outside the image", c->mod);
    uint32_t a = rd32(c->img + init + ADRP_AT), w = rd32(c->img + init + LOAD_AT);
    if ((a & 0x9F00001F) != (0x90000000 | REG) || (w & 0xFFC003FF) != (0xB9400008 | REG << 5)) return BAD(c, "%s: DT_INIT is not the expected code", c->mod);
    int64_t at = (int64_t)((init + ADRP_AT) & ~(uint64_t)4095) + adrp_pages(a) * 4096 + (int64_t)(w >> 10 & 0xFFF) * 4;
    if (at < 0 || (uint64_t)at < c->l->data_off || (uint64_t)at >= c->l->key_page_off) return BAD(c, "%s: DT_INIT flag outside the data", c->mod);
    *flag = (uint64_t)at;
    return TOKT_OK;
}

/* main's PLT stub addressing the slot: it must be the only one in the last 64 KB of the text. */
static int find_probe_plt(Ctx *c, uint64_t *plt) {
    uint64_t end = c->l->text_end, found = 0;
    unsigned hits = 0;
    if (end < 0x10000) return BAD(c, "main: text too short for the PLT probe");
    for (uint64_t at = end - 0x10000; at < end - 12; at += 4) {
        if (!in_image(c, at, 16)) return BAD(c, "main: PLT scan outside the image");
        uint32_t a = rd32(c->img + at), b = rd32(c->img + at + 4), d = rd32(c->img + at + 12);
        if ((a & 0x9F00001F) != 0x90000010 || (b & 0xFFC003FF) != 0xF9400211 || d != 0xD61F0220) continue;
        int64_t ref = (int64_t)(at & ~(uint64_t)4095) + adrp_pages(a) * 4096 + (int64_t)(b >> 10 & 0xFFF) * 8;
        if (ref == (int64_t)c->probe_slot) { found = at; hits++; }
    }
    if (hits != 1) return BAD(c, "main: %u PLT stubs for the probe slot", hits);
    *plt = found;
    return TOKT_OK;
}

/* One module: its arrays go to out, its record to rec. */
static int module_tables(Ctx *c, const tokt_module *m, unsigned index, uint8_t rec[RECORD]) {
    const tokp_result *r = m->rewrite;
    const tokp_layout *l = &r->layout;
    char digest[65];
    uint64_t ctor = 0;
    int rc;
    c->mod = tokt_module_names[index];
    c->img = m->image; c->size = r->image_size; c->l = l;
    if (l->text_end > l->ro_off || l->ro_off > l->data_off || l->data_off > l->key_page_off || l->key_page_off > l->patch_off || l->patch_off > r->image_size)
        return BAD(c, "%s: image layout out of order", c->mod);
    if ((rc = read_tags(c)) || (rc = symbol_count(c, m))) return rc;
    if (c->tag[DT_SYMENT] != ENTRY || c->tag[DT_RELAENT] != ENTRY || c->tag[DT_PLTREL] != 7) return BAD(c, "%s: unexpected ELF entry sizes", c->mod);
    size_t symbols = c->out.n;
    if ((rc = emit_symbols(c))) return rc;
    size_t relocs = c->out.n;
    c->seen = calloc((size_t)((l->key_page_off - l->data_off) / 8 + 1), 1);
    if (!c->seen) return fail(c->err, c->errlen, TOKT_E_NOMEM, "out of memory");
    c->want_probe = index == TOKT_MAIN;
    rc = emit_relr(c);
    if (!rc) rc = emit_rela(c, c->tag[DT_RELA], c->tag[DT_RELASZ]);
    if (!rc) rc = emit_rela(c, c->tag[DT_JMPREL], c->tag[DT_PLTRELSZ]);
    free(c->seen); c->seen = NULL;
    if (rc) return rc;
    size_t gates = c->out.n;
    if ((rc = emit_gates(c, r)) || (rc = find_ctor_flag(c, &ctor))) return rc;
    if (m->image_sha256) memcpy(digest, m->image_sha256, 65);
    else tokt_sha256_hex(m->image, (size_t)r->image_size, digest);

    memset(rec, 0, RECORD);
    memcpy(rec, c->mod, strlen(c->mod));
    memcpy(rec + 16, digest, 64);
    const uint64_t q[13] = {r->image_size, l->text_end, l->ro_off, l->data_off, l->patch_off, l->key_page_off, c->tag[DT_INIT], ctor,
                            has(c, DT_INIT_ARRAY) ? c->tag[DT_INIT_ARRAY] : 0, has(c, DT_INIT_ARRAYSZ) ? c->tag[DT_INIT_ARRAYSZ] : 0, symbols, relocs, gates};
    for (int i = 0; i < 13; i++) wr64(rec + 88 + 8 * i, q[i]);
    wr32(rec + 192, c->nsym); wr32(rec + 196, (uint32_t)((gates - relocs) / ENTRY)); wr32(rec + 200, (uint32_t)((c->out.n - gates) / ENTRY));
    return TOKT_OK;
}

static int hex_digest(const char *s) {
    for (int i = 0; i < 64; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return !s[64];
}

int tokt_make(const tokt_module modules[TOKT_MODULE_COUNT], uint8_t **tables, size_t *size, char *err, size_t errlen) {
    if (!modules || !tables || !size) return fail(err, errlen, TOKT_E_ARGS, "no modules");
    *tables = NULL; *size = 0;
    for (int i = 0; i < TOKT_MODULE_COUNT; i++) {
        const tokt_module *m = &modules[i];
        if (!m->nso || !m->image || !m->rewrite || (m->image_sha256 && !hex_digest(m->image_sha256)))
            return fail(err, errlen, TOKT_E_ARGS, "%s: incomplete module", tokt_module_names[i]);
    }
    Ctx c;
    memset(&c, 0, sizeof c);
    c.err = err; c.errlen = errlen; c.mod = "tables";
    uint8_t records[TOKT_MODULE_COUNT][RECORD];
    uint64_t plt = 0;
    int rc = room(&c, &c.out, HEADER + RECORD * TOKT_MODULE_COUNT);
    if (rc) goto out;
    memset(c.out.p, 0, HEADER + RECORD * TOKT_MODULE_COUNT);
    c.out.n = HEADER + RECORD * TOKT_MODULE_COUNT;
    for (unsigned i = 0; i < TOKT_MODULE_COUNT; i++) {
        if ((rc = module_tables(&c, &modules[i], i, records[i]))) goto out;
        if (i != TOKT_MAIN) continue;
        if (!c.found_probe) { rc = BAD(&c, "main: no %s slot", "GetSystemTick"); goto out; }
        if ((rc = find_probe_plt(&c, &plt))) goto out;
        c.want_probe = 0;
    }
    size_t strings = c.out.n;
    if ((rc = room(&c, &c.out, c.names.n))) goto out;
    if (c.names.n) memcpy(c.out.p + strings, c.names.p, c.names.n);
    c.out.n += c.names.n;

    uint8_t *h = c.out.p;
    memcpy(h, "TOTKTAB1", 8);
    memcpy(h + 8, TOKT_VERSION, sizeof TOKT_VERSION - 1);
    wr32(h + 16, TOKT_MODULE_COUNT); wr32(h + 20, 1);
    wr64(h + 24, plt); wr64(h + 32, c.probe_slot); wr64(h + 40, strings); wr64(h + 48, c.names.n); wr64(h + 56, c.out.n);
    memcpy(h + HEADER, records, sizeof records);
    *tables = c.out.p; *size = c.out.n;
    c.out.p = NULL;
out:
    free(c.out.p); free(c.names.p);
    return rc;
}

int tokt_check_main(const void *nso, size_t nso_size, char *err, size_t errlen) {
    char hex[65];
    if (!nso) return fail(err, errlen, TOKT_E_ARGS, "no module");
    tokt_sha256_hex(nso, nso_size, hex);
    if (strcmp(hex, TOKT_MAIN_NSO_SHA256)) return fail(err, errlen, TOKT_E_VERSION, "this main module is not the supported 1.4.3 one");
    return TOKT_OK;
}

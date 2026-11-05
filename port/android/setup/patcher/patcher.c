/* The rewrite itself: patch_nso.py with DM_TARGET=android DM_X18=native. Each emit sequence below is the Python one, in the
   same order; regress.sh proves the bytes. x18 stays the game's, so only svc, the thread registers and the timers change.
   Capstone 5.0.9 decodes each site, as dis() does in the Python tool. In this mode that is its only use and it cannot refuse
   a site: tests/decode_check.py decodes every word the scan can pick. The x18 windows, where decoding matters, are not ported. */
#include "patcher_internal.h"
#include <capstone/capstone.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Node visits allowed in the exclusive-region check. The game's modules take under a thousand; this stops crafted ones. */
#define RESERVATION_BUDGET (1ull << 28)
/* tokp_rewrite_alloc's first try; 1.4.3 main needs 48 KB */
#define PATCH_ROOM (256u << 10)

const char *const tokp_kind_names[TOKP_KIND_COUNT] = {"svc", "tpidr_r", "tpidr_w", "tpidrro_r", "cntpct", "cntvct"};

/* Key page indices (patch_nso.py KEY_INDEX) and the slot of the host context entry. */
enum { K18 = 0, KTLS = 1, KTLR = 2, KGATE = 3, KEY_CONTEXT_ENTRY = 8 };

int tokp_fail(char *err, size_t errlen, int code, const char *fmt, ...) {
    if (err && errlen) { va_list a; va_start(a, fmt); vsnprintf(err, errlen, fmt, a); va_end(a); }
    return code;
}

typedef struct {
    uint8_t *img; uint64_t cap;
    /* patch: bytes emitted, counted past cap too */
    uint64_t key, patch_off, patch;
    /* first encoding that cannot be made */
    const char *bad;
    uint64_t bad_at;
    /* room for every gate site */
    tokp_gate *gates; size_t ngates;
    uint32_t slot_refs;
} Rw;

static uint64_t here(const Rw *r) { return r->patch_off + r->patch; }
static void refuse(Rw *r, const char *why, uint64_t at) { if (!r->bad) { r->bad = why; r->bad_at = at; } }
static void emit(Rw *r, uint32_t w) {
    if (here(r) + 4 <= r->cap) wr32(r->img + here(r), w);
    r->patch += 4;
}

/* ---- encoders ---- */
static uint32_t B(Rw *r, uint64_t from, uint64_t to) {
    int64_t off = (int64_t)to - (int64_t)from;
    if (off < -(1 << 27) || off >= (1 << 27) || off % 4) { refuse(r, "branch out of range", from); return 0; }
    return 0x14000000 | ((uint32_t)(off >> 2) & 0x3FFFFFF);
}
static uint32_t ADRP(Rw *r, unsigned d, uint64_t pc, uint64_t target) {
    int64_t imm = ((int64_t)(target & ~0xFFFull) - (int64_t)(pc & ~0xFFFull)) / 4096;
    if (imm < -(1 << 20) || imm >= (1 << 20)) { refuse(r, "ADRP out of range", pc); return 0; }
    return 0x90000000 | ((uint32_t)(imm & 3) << 29) | ((uint32_t)((imm >> 2) & 0x7FFFF) << 5) | d;
}
/* off is a multiple of 8 for the pair forms */
static uint32_t pair(uint32_t op, unsigned t, unsigned t2, unsigned n, int off) { return op | (((uint32_t)(off / 8) & 0x7F) << 15) | t2 << 10 | n << 5 | t; }
#define STP(t, t2, n, off) pair(0xA9000000, t, t2, n, off)
#define LDP(t, t2, n, off) pair(0xA9400000, t, t2, n, off)
#define STP_PRE(t, t2, n, off) pair(0xA9800000, t, t2, n, off)
#define LDP_POST(t, t2, n, off) pair(0xA8C00000, t, t2, n, off)
static uint32_t STR_PRE(unsigned t, unsigned n, int off) { return 0xF8000C00 | (((uint32_t)off & 0x1FF) << 12) | n << 5 | t; }
static uint32_t LDR_POST(unsigned t, unsigned n, int off) { return 0xF8400400 | (((uint32_t)off & 0x1FF) << 12) | n << 5 | t; }
static uint32_t LDRX(unsigned t, unsigned n, unsigned imm12) { return 0xF9400000 | imm12 << 10 | n << 5 | t; }
static uint32_t STRX(unsigned t, unsigned n, unsigned imm12) { return 0xF9000000 | imm12 << 10 | n << 5 | t; }
static uint32_t MOVR(unsigned d, unsigned m) { return 0xAA0003E0 | m << 16 | d; }
static uint32_t ADDI(unsigned d, unsigned n, unsigned imm12) { return 0x91000000 | imm12 << 10 | n << 5 | d; }
static uint32_t SUBI(unsigned d, unsigned n, unsigned imm12) { return 0xD1000000 | imm12 << 10 | n << 5 | d; }
static uint32_t ADDR(unsigned d, unsigned n, unsigned m) { return 0x8B000000 | m << 16 | n << 5 | d; }
static uint32_t BLR(unsigned n) { return 0xD63F0000 | n << 5; }

static void emit_key_addr(Rw *r, unsigned reg, unsigned key) {
    r->slot_refs++;
    emit(r, ADRP(r, reg, here(r), r->key)); emit(r, LDRX(reg, reg, key));
}
static void emit_slot_addr(Rw *r, unsigned base, unsigned key_reg, unsigned key) {
    emit_key_addr(r, key_reg, key); emit(r, ADDR(base, base, key_reg));
}
static void emit_back(Rw *r, uint64_t a) { emit(r, B(r, here(r), a + 4)); }

/* xt = this thread's slot block, through the host context entry (which changes only x16, NZCV and FP state preserved).
   Android has no red zone: pre-index pushes, post-index pops, nothing below SP. */
static void thread_base(Rw *r, unsigned t, uint64_t a) {
    if (t == 16) {
        emit(r, STR_PRE(30, 31, -16)); emit(r, ADRP(r, 16, here(r), r->key)); emit(r, LDRX(16, 16, KEY_CONTEXT_ENTRY)); emit(r, BLR(16));
        emit(r, LDR_POST(30, 31, 16));
    } else if (t == 30) {
        emit(r, STR_PRE(16, 31, -16)); emit(r, ADRP(r, 16, here(r), r->key)); emit(r, LDRX(16, 16, KEY_CONTEXT_ENTRY)); emit(r, BLR(16));
        emit(r, MOVR(30, 16)); emit(r, LDR_POST(16, 31, 16));
    } else {
        if (t >= 31 || t == 18) refuse(r, "no thread base for this register", a);
        emit(r, STP_PRE(16, 30, 31, -16)); emit(r, ADRP(r, 16, here(r), r->key)); emit(r, LDRX(16, 16, KEY_CONTEXT_ENTRY)); emit(r, BLR(16));
        emit(r, MOVR(t, 16)); emit(r, LDP_POST(16, 30, 31, 16));
    }
}

/* ---- the three block shapes ---- */
/* mrs xN, tpidr_el0 / tpidrro_el0 -> xN = thread slot. x18 as the target goes to the K18 slot, as the Python tool does in
   every mode; no game module has one. */
static uint64_t site_sysreg_read(Rw *r, uint32_t w, uint64_t a, unsigned key) {
    unsigned N = w & 31;
    uint64_t start = here(r);
    if (N == 18) {
        const unsigned S = 0, T = 1, Nf = 32;
        emit(r, SUBI(31, 31, Nf)); emit(r, STP(S, T, 31, 0)); thread_base(r, T, a);
        emit_slot_addr(r, T, S, key); emit(r, LDRX(S, T, 0)); emit(r, STRX(S, 31, 2));
        thread_base(r, T, a); emit_slot_addr(r, T, S, K18); emit(r, LDRX(S, 31, 2)); emit(r, STRX(S, T, 0));
        emit(r, LDP(S, T, 31, 0)); emit(r, ADDI(31, 31, Nf)); emit_back(r, a);
        return start;
    }
    unsigned tmp = N != 16 ? 16 : 17;
    emit(r, SUBI(31, 31, 16)); emit(r, STRX(tmp, 31, 0)); thread_base(r, N, a); emit_slot_addr(r, N, tmp, key);
    emit(r, LDRX(N, N, 0)); emit(r, LDRX(tmp, 31, 0)); emit(r, ADDI(31, 31, 16)); emit_back(r, a);
    return start;
}

/* msr tpidr_el0, xN */
static uint64_t site_tpidr_write(Rw *r, uint32_t w, uint64_t a) {
    unsigned N = w & 31, T = N == 0 ? 1 : 0, tmp = N == 16 ? 17 : 16;
    uint64_t start = here(r);
    if (N == 18) {
        /* the first register that is none of T, tmp, N; T is 0 here */
        const unsigned U = 1, Nf = 32;
        emit(r, SUBI(31, 31, Nf)); emit(r, STP(T, tmp, 31, 0)); emit(r, STRX(U, 31, 2));
        thread_base(r, T, a); emit_slot_addr(r, T, tmp, KTLS);
        thread_base(r, tmp, a); emit_slot_addr(r, tmp, U, K18); emit(r, LDRX(tmp, tmp, 0)); emit(r, STRX(tmp, T, 0));
        emit(r, LDP(T, tmp, 31, 0)); emit(r, LDRX(U, 31, 2)); emit(r, ADDI(31, 31, Nf)); emit_back(r, a);
        return start;
    }
    emit(r, SUBI(31, 31, 16)); emit(r, STP(T, tmp, 31, 0));
    thread_base(r, T, a); emit_slot_addr(r, T, tmp, KTLS); emit(r, STRX(N, T, 0));
    emit(r, LDP(T, tmp, 31, 0)); emit(r, ADDI(31, 31, 16)); emit_back(r, a);
    return start;
}

/* svc / mrs cntpct_el0 / cntvct_el0 -> host gate, which finds its gates entry by the return address */
static uint64_t site_gate(Rw *r, uint64_t a, uint32_t kind, uint32_t arg) {
    uint64_t start = here(r);
    emit(r, STP_PRE(29, 30, 31, -16));
    thread_base(r, 30, a); emit_slot_addr(r, 30, 29, KGATE); emit(r, LDRX(30, 30, 0)); emit(r, LDRX(29, 31, 0));
    emit(r, BLR(30));
    uint64_t ret = here(r);
    emit(r, LDP_POST(29, 30, 31, 16));
    emit_back(r, a);
    r->gates[r->ngates++] = (tokp_gate){ret, a, kind, arg};
    return start;
}

/* ---- finding the sites: a linear scan, in the Python tool's order of tests ---- */
static int scan(const uint8_t *img, uint64_t text_end, tokp_site **out, size_t *count) {
    tokp_site *v = NULL;
    size_t n = 0, cap = 0;
    for (uint64_t o = 0; o < text_end; o += 4) {
        if (img[o + 3] != 0xD4 && img[o + 3] != 0xD5) continue;
        uint32_t w = rd32(img + o), kind, arg = 0;
        if ((w & 0xFFE0001F) == 0xD4000001) { kind = TOKP_SVC; arg = (w >> 5) & 0xFFFF; }
        else if ((w & 0xFFFFFFE0) == 0xD53BD040) kind = TOKP_TPIDR_R;
        else if ((w & 0xFFFFFFE0) == 0xD51BD040) kind = TOKP_TPIDR_W;
        else if ((w & 0xFFFFFFE0) == 0xD53BD060) kind = TOKP_TPIDRRO_R;
        else if ((w & 0xFFFFFFE0) == 0xD53BE020) { kind = TOKP_CNTPCT; arg = w & 31; }
        else if ((w & 0xFFFFFFE0) == 0xD53BE040) { kind = TOKP_CNTVCT; arg = w & 31; }
        else continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 1024;
            tokp_site *p = realloc(v, cap * sizeof *p);
            if (!p) { free(v); return -1; }
            v = p;
        }
        v[n++] = (tokp_site){o, kind, arg};
    }
    *out = v; *count = n;
    return 0;
}

/* Every site must decode to one instruction, as dis() asserts. */
static int decode_sites(const uint8_t *img, const tokp_site *sites, size_t n, char *err, size_t errlen) {
    csh cs;
    if (!n) return TOKP_OK;
    if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &cs) != CS_ERR_OK) return tokp_fail(err, errlen, TOKP_E_DECODER, "Capstone will not open");
    cs_option(cs, CS_OPT_DETAIL, CS_OPT_ON);
    cs_insn *ins = cs_malloc(cs);
    int rc = ins ? TOKP_OK : tokp_fail(err, errlen, TOKP_E_NOMEM, "out of memory");
    for (size_t i = 0; rc == TOKP_OK && i < n; i++) {
        const uint8_t *p = img + sites[i].off;
        size_t left = 4;
        uint64_t addr = sites[i].off;
        if (!cs_disasm_iter(cs, &p, &left, &addr, ins) || left) rc = tokp_fail(err, errlen, TOKP_E_DECODER, "no instruction at site %#llx", (unsigned long long)sites[i].off);
    }
    if (ins) cs_free(ins, 1);
    cs_close(&cs);
    return rc;
}

/* ---- exclusive regions (reservation_committed_sites) ----
   A rewrite block's calls and memory accesses clear the exclusive monitor, so a site between a load-exclusive and the
   store-exclusive it can commit would make that store fail forever: refuse the module. Forward from each load-exclusive,
   stopping at exclusive loads and stores and clrex, within its function; then backward from the stores found. The Python
   tool walks every function; a function with no site cannot give a hit, so only those are walked here. */
static int is_ldx(uint32_t w) { return (w & 0x3F800000) == 0x08000000 && (w >> 22 & 1) && !(w >> 31 == 0 && (w >> 21 & 1)); }
static int is_stx(uint32_t w) { return (w & 0x3F800000) == 0x08000000 && !(w >> 22 & 1) && !(w >> 31 == 0 && (w >> 21 & 1)); }
static int is_clrex(uint32_t w) { return (w & 0xFFFFF0FF) == 0xD503305F; }
static int is_barrier(uint32_t w) { return is_ldx(w) || is_stx(w) || is_clrex(w); }
static int64_t sx(uint32_t v, int b) { return (v >> (b - 1) & 1) ? (int64_t)v - ((int64_t)1 << b) : (int64_t)v; }

/* Successors of the instruction at o; returns how many, in the Python tool's order. */
static int succ(uint32_t w, int64_t o, int64_t s[2]) {
    if ((w & 0xFC000000) == 0x14000000) { s[0] = o + 4 * sx(w & 0x3FFFFFF, 26); return 1; }
    if ((w & 0xFF000010) == 0x54000000 || (w & 0x7E000000) == 0x34000000) { s[0] = o + 4; s[1] = o + 4 * sx((w >> 5) & 0x7FFFF, 19); return 2; }
    if ((w & 0x7E000000) == 0x36000000) { s[0] = o + 4; s[1] = o + 4 * sx((w >> 5) & 0x3FFF, 14); return 2; }
    if ((w & 0xFFFFFC1F) == 0xD61F0000 || (w & 0xFFFFFC1F) == 0xD65F0000) return 0;
    s[0] = o + 4;
    return 1;
}

/* Work arrays over the words of one function, sized for the largest walked. */
typedef struct {
    size_t words;
    /* == gen: in the forward set */
    uint32_t *mark;
    /* == gen: in the backward set */
    uint32_t *back;
    /* the forward set, as word indices */
    uint32_t *list;
    /* two entries per word */
    uint32_t *stack;
    /* per word: first incoming edge + 1 */
    uint32_t *first;
    /* per edge, two per word */
    uint32_t *next, *from;
    /* per word: a site is here */
    uint8_t *site;
    uint32_t gen;
    uint64_t visits;
} Walk;

static void walk_free(Walk *k) { free(k->mark); free(k->back); free(k->list); free(k->stack); free(k->first); free(k->next); free(k->from); free(k->site); }

static int walk_size(Walk *k, size_t words) {
    if (words <= k->words) return 0;
    uint32_t gen = k->gen;
    uint64_t visits = k->visits;
    walk_free(k);
    memset(k, 0, sizeof *k);
    k->gen = gen; k->visits = visits;
    k->words = words;
    k->mark = calloc(words, 4); k->back = calloc(words, 4); k->list = malloc(words * 4); k->stack = malloc(words * 8 + 16);
    k->first = malloc(words * 4); k->next = malloc(words * 8); k->from = malloc(words * 8); k->site = malloc(words);
    return k->mark && k->back && k->list && k->stack && k->first && k->next && k->from && k->site ? 0 : -1;
}

/* Is a site on a committing path of the load-exclusive at L? If so returns 1 with its address in *hit. */
static int walk_load(Walk *k, const uint8_t *img, uint64_t fa, uint64_t fb, uint64_t L, uint64_t *hit) {
    size_t nf = 0, sp = 0, nedge = 0;
    int64_t s[2];
    uint32_t gen = ++k->gen;
    int found = 0, n = succ(rd32(img + L), (int64_t)L, s);
    for (int i = 0; i < n; i++) if (s[i] >= (int64_t)fa && s[i] < (int64_t)fb) k->stack[sp++] = (uint32_t)(((uint64_t)s[i] - fa) / 4);
    while (sp) {
        uint32_t x = k->stack[--sp];
        if (k->mark[x] == gen) continue;
        k->mark[x] = gen; k->list[nf++] = x; k->visits++;
        found |= k->site[x];
        uint64_t o = fa + (uint64_t)x * 4;
        uint32_t w = rd32(img + o);
        if (is_barrier(w)) continue;
        n = succ(w, (int64_t)o, s);
        for (int i = 0; i < n; i++) {
            if (s[i] < (int64_t)fa || s[i] >= (int64_t)fb) continue;
            uint32_t y = (uint32_t)(((uint64_t)s[i] - fa) / 4);
            if (k->mark[y] != gen) k->stack[sp++] = y;
        }
    }
    if (!found) return 0;
    for (size_t i = 0; i < nf; i++) k->first[k->list[i]] = 0;
    for (size_t i = 0; i < nf; i++) {
        uint32_t x = k->list[i];
        uint64_t o = fa + (uint64_t)x * 4;
        uint32_t w = rd32(img + o);
        if (is_barrier(w)) continue;
        n = succ(w, (int64_t)o, s);
        for (int j = 0; j < n; j++) {
            if (s[j] < (int64_t)fa || s[j] >= (int64_t)fb) continue;
            uint32_t y = (uint32_t)(((uint64_t)s[j] - fa) / 4);
            if (k->mark[y] != gen) continue;
            k->from[nedge] = x; k->next[nedge] = k->first[y]; k->first[y] = (uint32_t)++nedge;
        }
    }
    for (size_t i = 0; i < nf; i++) {
        uint32_t x = k->list[i];
        if (is_stx(rd32(img + fa + (uint64_t)x * 4))) { k->back[x] = gen; k->stack[sp++] = x; }
    }
    while (sp) {
        uint32_t x = k->stack[--sp];
        k->visits++;
        if (k->site[x]) { *hit = fa + (uint64_t)x * 4; return 1; }
        for (uint32_t e = k->first[x]; e; e = k->next[e - 1]) {
            uint32_t p = k->from[e - 1];
            if (k->back[p] != gen) { k->back[p] = gen; k->stack[sp++] = p; }
        }
    }
    return 0;
}

static int check_reservations(const uint8_t *img, uint64_t text_end, const uint64_t *starts, size_t nstarts,
                              const tokp_site *sites, size_t nsites, char *err, size_t errlen) {
    Walk k;
    size_t si = 0;
    int rc = TOKP_OK;
    memset(&k, 0, sizeof k);
    for (size_t f = 0; f <= nstarts && si < nsites && rc == TOKP_OK; f++) {
        uint64_t fa = f ? starts[f - 1] : 0, fb = f < nstarts ? starts[f] : text_end;
        while (si < nsites && sites[si].off < fa) si++;
        size_t first = si;
        while (si < nsites && sites[si].off < fb) si++;
        /* no site here; from an odd start no walk can land on one */
        if (first == si || fa % 4) continue;
        size_t words = (size_t)((fb - fa + 3) / 4);
        if (walk_size(&k, words)) { rc = tokp_fail(err, errlen, TOKP_E_NOMEM, "out of memory"); break; }
        memset(k.site, 0, words);
        for (size_t i = first; i < si; i++) k.site[(sites[i].off - fa) / 4] = 1;
        for (uint64_t L = fa; L < fb && rc == TOKP_OK; L += 4) {
            if (!is_ldx(rd32(img + L))) continue;
            uint64_t hit;
            if (walk_load(&k, img, fa, fb, L, &hit)) rc = tokp_fail(err, errlen, TOKP_E_RULE, "rewrite site %#llx is inside the exclusive region of %#llx",
                                    (unsigned long long)hit, (unsigned long long)L);
            else if (k.visits > RESERVATION_BUDGET) rc = tokp_fail(err, errlen, TOKP_E_RULE, "exclusive regions too large to check");
        }
    }
    walk_free(&k);
    return rc;
}

int tokp_rewrite(const void *nso, size_t nso_size, void *image, size_t image_cap, tokp_result *out, char *err, size_t errlen) {
    uint8_t *img = image;
    uint64_t *starts = NULL;
    size_t nstarts = 0;
    Rw r;
    int rc;
    if (!image || !out) return tokp_fail(err, errlen, TOKP_E_ARGS, "no image or result");
    memset(out, 0, sizeof *out);
    memset(&r, 0, sizeof r);
    if ((rc = tokp_read_layout(nso, nso_size, &out->layout, err, errlen))) return rc;
    const tokp_layout *l = &out->layout;
    if (image_cap < l->patch_off) {
        out->image_size = l->patch_off;
        return tokp_fail(err, errlen, TOKP_E_SPACE, "the image needs %#llx bytes before its patch area", (unsigned long long)l->patch_off);
    }
    if ((rc = tokp_load_image(nso, l, img, err, errlen))) goto fail;
    if ((rc = tokp_function_starts(img, l->patch_off, l->text_end, &starts, &nstarts, err, errlen))) goto fail;
    if (scan(img, l->text_end, &out->sites, &out->nsites)) { rc = tokp_fail(err, errlen, TOKP_E_NOMEM, "out of memory"); goto fail; }
    if ((rc = decode_sites(img, out->sites, out->nsites, err, errlen))) goto fail;
    if ((rc = check_reservations(img, l->text_end, starts, nstarts, out->sites, out->nsites, err, errlen))) goto fail;

    r.img = img; r.cap = image_cap; r.key = l->key_page_off; r.patch_off = l->patch_off;
    size_t gate_sites = 0;
    for (size_t i = 0; i < out->nsites; i++) gate_sites += out->sites[i].kind == TOKP_SVC || out->sites[i].kind >= TOKP_CNTPCT;
    if (gate_sites && !(r.gates = malloc(gate_sites * sizeof *r.gates))) { rc = tokp_fail(err, errlen, TOKP_E_NOMEM, "out of memory"); goto fail; }
    for (size_t i = 0; i < out->nsites; i++) {
        const tokp_site *s = &out->sites[i];
        uint32_t w = rd32(img + s->off);
        uint64_t st;
        switch (s->kind) {
            case TOKP_TPIDR_R: st = site_sysreg_read(&r, w, s->off, KTLS); break;
            case TOKP_TPIDRRO_R: st = site_sysreg_read(&r, w, s->off, KTLR); break;
            case TOKP_TPIDR_W: st = site_tpidr_write(&r, w, s->off); break;
            default: st = site_gate(&r, s->off, s->kind, s->arg); break;
        }
        wr32(img + s->off, B(&r, s->off, st));
    }
    if (r.bad) { rc = tokp_fail(err, errlen, TOKP_E_RULE, "%s at %#llx", r.bad, (unsigned long long)r.bad_at); goto fail; }
    free(starts);
    out->patch_size = r.patch;
    out->image_size = l->patch_off + r.patch;
    out->gates = r.gates; out->ngates = r.ngates;
    out->slot_refs = r.slot_refs;
    if (out->image_size > image_cap) {
        uint64_t size = out->image_size, patch = out->patch_size;
        tokp_layout keep = out->layout;
        tokp_result_free(out);
        out->layout = keep; out->image_size = size; out->patch_size = patch;
        return tokp_fail(err, errlen, TOKP_E_SPACE, "the image needs %#llx bytes", (unsigned long long)size);
    }
    return TOKP_OK;
fail:
    free(starts); free(r.gates);
    tokp_result_free(out);
    return rc;
}

int tokp_rewrite_alloc(const void *nso, size_t nso_size, uint8_t **image, tokp_result *out, char *err, size_t errlen) {
    tokp_layout l;
    int rc;
    if (!image || !out) return tokp_fail(err, errlen, TOKP_E_ARGS, "no image or result");
    *image = NULL;
    if ((rc = tokp_read_layout(nso, nso_size, &l, err, errlen))) return rc;
    size_t cap = (size_t)l.patch_off + PATCH_ROOM;
    for (int pass = 0; pass < 2; pass++) {
        uint8_t *p = malloc(cap);
        if (!p) return tokp_fail(err, errlen, TOKP_E_NOMEM, "out of memory");
        rc = tokp_rewrite(nso, nso_size, p, cap, out, err, errlen);
        if (rc == TOKP_OK) { *image = p; return rc; }
        free(p);
        if (rc != TOKP_E_SPACE) return rc;
        cap = (size_t)out->image_size;
    }
    return rc;
}

void tokp_result_free(tokp_result *r) {
    if (!r) return;
    free(r->sites); free(r->gates);
    memset(r, 0, sizeof *r);
}

/* The "A Grab" bubble and the HUD's ability key (2026-10-27): fixed art the game draws with the Switch's buttons, so it is
   rewritten as the game opens its files (runtime/romfs.c generator) to show the key that does the action on this device
   (dm_android_prompt_places, host/app.c). All edits are in place (same sizes: the resource size table stays right).
   - The command bubble (UI layout DoCommand_00 in UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs) has one
     diamond per button the game asks for (N_A_00 talk / grab..., N_B_00 dash, N_X_00 jump, N_Y_00 attacks), each with its
     key where that button sits on a Switch (A right, B bottom, X top, Y left), its verb box beside it, three small ring
     letters at the other places, and rush / hold effects next to the key. The game picks the diamond and writes the verb
     into it; here each diamond is rearranged: the key (and its verb box, mirrored when it changes sides as the Y diamond
     is drawn) moves to the place of the key that does the action, the effects follow, the ring letters take the other
     three places, and every letter is the button style's at its place (TOTK_BUTTON_STYLE: Xbox right B, bottom A, top Y,
     left X; Switch right A, bottom B, top X, left Y).
   - The ring letters' text comes from LayoutMsg/DoCommand_00.msbt (by pane name) and the HUD's ability key from
     LayoutMsg/PlayerStatus_00.msbt (Pa_ShortCutGuide_00-T_Icon_00, a button-font character), in Mals/<language>.sarc.zs.
   Archives are read with zstd dictionary 1 (zs.zsdic in Pack/ZsDic.pack.zs) and served uncompressed (raw zstd blocks).
   The files may come from a mod (romfs.c passes the overlay's bytes): every offset is checked against its section, and a
   file that does not hold together is served unchanged.
   The two other rules here need no setting: the quick menus' capture timing and the render-target heaps. They take the place
   of the PC's overlays (romfs-patches, romfs-patches-out), so a game started from the player's own files needs neither. */
#include "runtime.h"
#include "player_files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ZSTD_STATIC_LINKING_ONLY   /* ZSTD_BLOCKSIZE_MAX */
#include <zstd.h>

/* host/app.c: per diamond A, B, X, Y the place (0 right, 1 bottom, 2 top, 3 left) of the key that does its action, and
   the button-font character of the abilities' key (0: the game's); 0 = leave the art as it is */
int dm_android_prompt_places(int8_t places[4], uint16_t *ability_char, char letters[4]);
int dm_android_prompt_glyphs(void);   /* 1: the style has symbols, not letters (PlayStation) */

enum { RIGHT, BOTTOM, TOP, LEFT };
static const float kKeyAt[4][2] = {{39, 0}, {0, -39}, {0, 39}, {-39, 0}};      /* the key containers' places in a diamond */
static const float kRingAt[4][2] = {{39, 1}, {2, -39}, {1, 39}, {-38, 0}};     /* the ring letters' */
static int switch_place(char letter) { return letter == 'A' ? RIGHT : letter == 'B' ? BOTTOM : letter == 'X' ? TOP : LEFT; }
static int group_index(char letter) { return letter == 'A' ? 0 : letter == 'B' ? 1 : letter == 'X' ? 2 : letter == 'Y' ? 3 : -1; }

/* BFLYT fields used here, from the start of a section (its 8-byte header included). Every pane section (pan1, pic1, txt1,
   wnd1, prt1) starts with the pane: origin bits, name, translation, rotation, scale, size. pic1 continues with four
   vertex colours and its material; txt1 with buffer / string lengths, material, font and its alignment. A material
   (mat1 entry) has its name, flags (bits 0-1: how many texture maps) and colours before its first texture map. */
enum {
    PANE_ORIGIN = 0x09,   /* bits 0-1: horizontal anchor, 1 left / 2 right (0 centre) */
    PANE_NAME = 0x0c, PANE_NAME_LEN = 24, PANE_X = 0x2c, PANE_Y = 0x30, PANE_ROT_Z = 0x40, PANE_SCALE_X = 0x44, PANE_END = 0x54,
    PIC_MATERIAL = 0x64, PIC_END = 0x66, TXT_ALIGN = 0x5c, TXT_END = 0x5d,
    MAT_FLAGS = 0x1c, MAT_TEXTURE0 = 0x2c, MAT_END = 0x2e,
};
/* The raw frame finish_raw_frame builds: magic, frame header descriptor, 8-byte content size, then blocks of a 3-byte
   header and at most ZSTD_BLOCKSIZE_MAX bytes */
enum { RAW_FRAME_HEAD = 4 + 1 + 8, RAW_BLOCK_HEAD = 3 };
static uint64_t raw_blocks(uint64_t n) { return (n + ZSTD_BLOCKSIZE_MAX - 1) / ZSTD_BLOCKSIZE_MAX; }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static float rdf(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }
static void wrf(uint8_t *p, float f) { memcpy(p, &f, 4); }

/* A member of a little-endian SARC: its bytes, or NULL */
static uint8_t *sarc_member(uint8_t *sarc, uint64_t size, const char *name, uint64_t *out_size) {
    if (size < 0x14 || memcmp(sarc, "SARC", 4) || rd16(sarc + 6) != 0xfeff) return NULL;
    uint64_t data = rd32(sarc + 0xc), fat = rd16(sarc + 4);
    if (fat + 12 > size || memcmp(sarc + fat, "SFAT", 4)) return NULL;
    uint64_t n = rd16(sarc + fat + 6), nodes = fat + 12, fnt = nodes + 16 * n;
    if (fnt + 8 > size || memcmp(sarc + fnt, "SFNT", 4)) return NULL;
    uint64_t names = fnt + 8, want = strlen(name) + 1;
    for (uint64_t i = 0; i < n; i++) {
        const uint8_t *e = sarc + nodes + 16 * i; uint32_t attr = rd32(e + 4);
        uint64_t at = names + (uint64_t)(attr & 0xffffff) * 4;
        if (!(attr & 0x01000000) || at + want > size || memcmp(sarc + at, name, want)) continue;
        uint64_t a = rd32(e + 8), b = rd32(e + 12);
        if (data + b > size || b < a) return NULL;
        *out_size = b - a; return sarc + data + a;
    }
    return NULL;
}
static ZSTD_DDict *dictionary(void) {
    static ZSTD_DDict *dict; static int tried;
    if (tried) return dict;
    tried = 1; uint8_t *pack = NULL; uint64_t n = 0;
    if (runtime_romfs_load("Pack/ZsDic.pack.zs", &pack, &n)) return NULL;
    unsigned long long m = ZSTD_getFrameContentSize(pack, n);
    uint8_t *sarc = m && m < (1ull << 28) ? malloc(m) : NULL;
    if (sarc && ZSTD_decompress(sarc, m, pack, n) == m) {
        uint64_t dn; uint8_t *d = sarc_member(sarc, m, "zs.zsdic", &dn);
        if (d) dict = ZSTD_createDDict(d, dn);
    }
    free(sarc); free(pack); return dict;
}
/* Decompressed into a buffer laid out for the raw frame it becomes (finish_raw_frame); NULL on failure */
static uint8_t *open_archive(const uint8_t *data, uint64_t size, uint8_t **sarc, uint64_t *n) {
    ZSTD_DDict *dict = dictionary(); if (!dict) { fprintf(stderr, "[totk-prompts] no zstd dictionary: the art stays\n"); return NULL; }
    unsigned long long m = ZSTD_getFrameContentSize(data, size);
    if (m == ZSTD_CONTENTSIZE_UNKNOWN || m == ZSTD_CONTENTSIZE_ERROR || m > (1ull << 30)) return NULL;
    uint64_t skip = RAW_FRAME_HEAD + RAW_BLOCK_HEAD * raw_blocks(m);   /* room for the headers finish_raw_frame writes */
    uint8_t *frame = malloc(skip + m); if (!frame) return NULL;
    ZSTD_DCtx *dctx = ZSTD_createDCtx(); size_t r = ZSTD_decompress_usingDDict(dctx, frame + skip, m, data, size, dict); ZSTD_freeDCtx(dctx);
    if (ZSTD_isError(r) || r != m) { free(frame); return NULL; }
    *sarc = frame + skip; *n = m; return frame;
}
/* The archive as one zstd frame of raw blocks, built in place before its bytes; its size */
static uint64_t finish_raw_frame(uint8_t *frame, uint64_t n) {
    uint64_t blocks = raw_blocks(n);
    frame[0] = 0x28; frame[1] = 0xb5; frame[2] = 0x2f; frame[3] = 0xfd; frame[4] = 0xe0;   /* single segment, 8-byte content size, no dictionary */
    for (int i = 0; i < 8; i++) frame[5 + i] = (uint8_t)(n >> (8 * i));
    uint8_t *o = frame + RAW_FRAME_HEAD; const uint8_t *src = frame + RAW_FRAME_HEAD + RAW_BLOCK_HEAD * blocks;
    for (uint64_t b = 0; b < blocks; b++) {   /* each block lands before its source: moving left is safe */
        uint64_t left = n - b * ZSTD_BLOCKSIZE_MAX, len = left < ZSTD_BLOCKSIZE_MAX ? left : ZSTD_BLOCKSIZE_MAX; uint32_t h = (uint32_t)(len << 3) | (b + 1 == blocks);
        o[0] = (uint8_t)h; o[1] = (uint8_t)(h >> 8); o[2] = (uint8_t)(h >> 16);
        memmove(o + RAW_BLOCK_HEAD, src, len); o += RAW_BLOCK_HEAD + len; src += len;
    }
    return (uint64_t)(o - frame);
}

/* ---- the layout ---- */
typedef struct { int8_t places[4]; char letters[4]; int tex[4], base[2]; int moved, mirrored, keys, bases; char stack[16][25]; int depth; char last[25]; uint8_t *mat1; uint64_t mat1_size; } layout_ctx;
static int mirror_group(const layout_ctx *c, int g) { return (switch_place("ABXY"[g]) == LEFT) != (c->places[g] == LEFT); }
static const char *ancestor(const layout_ctx *c, const char *prefix) {
    for (int i = c->depth - 1; i >= 0; i--) if (!strncmp(c->stack[i], prefix, strlen(prefix))) return c->stack[i];
    return NULL;
}
static int pane_group(const layout_ctx *c) {   /* the diamond (N_A_00 ...) the pane is in, or -1 */
    for (int i = 0; i < c->depth; i++) { const char *s = c->stack[i];
        if (s[0] == 'N' && s[1] == '_' && s[3] == '_' && !strcmp(s + 4, "00") && group_index(s[2]) >= 0) return group_index(s[2]); }
    return -1;
}
static int in_key_container(const layout_ctx *c) { return ancestor(c, "N_Btn") || ancestor(c, "N_CompanionBtn"); }
static void set_key_texture(layout_ctx *c, uint8_t *pane, uint64_t size, int g) {   /* a key picture: the letter at the diamond's new place */
    if (!c->mat1 || size < PIC_END || c->mat1_size < 12) return;
    unsigned index = rd16(pane + PIC_MATERIAL); if (index >= rd16(c->mat1 + 8) || 12 + 4 * (uint64_t)index + 4 > c->mat1_size) return;
    uint64_t at = rd32(c->mat1 + 12 + 4 * index); if (at + MAT_END > c->mat1_size) return;
    uint8_t *q = c->mat1 + at;
    if (!(rd32(q + MAT_FLAGS) & 3)) return;   /* no texture map */
    int t = rd16(q + MAT_TEXTURE0), letter = c->letters[c->places[g]];
    if (t != c->tex[0] && t != c->tex[1] && t != c->tex[2] && t != c->tex[3]) return;
    wr16(q + MAT_TEXTURE0, (uint16_t)c->tex[group_index((char)letter)]); c->keys++;
}
/* A diamond's backdrop (P_BaseA_00 ...): the dark shape with a big lobe under the key and small ones under the ring
   letters. The game draws it from two textures, the key's lobe on the right (Nt_KeyBtnSubBaseAL_00: A as it is, Y turned
   180 degrees) or at the bottom (Nt_KeyBtnSubBaseBL_00: B as it is, X turned and mirrored); the backdrop of a key that
   moved takes the texture, turn and mirroring the game uses for a key at its new place. */
static void set_backdrop(layout_ctx *c, uint8_t *pane, uint64_t size, int to) {
    if (!c->mat1 || size < PIC_END || c->mat1_size < 12 || c->base[0] < 0 || c->base[1] < 0) return;
    unsigned index = rd16(pane + PIC_MATERIAL); if (index >= rd16(c->mat1 + 8) || 12 + 4 * (uint64_t)index + 4 > c->mat1_size) return;
    uint64_t at = rd32(c->mat1 + 12 + 4 * index); if (at + MAT_END > c->mat1_size) return;
    uint8_t *q = c->mat1 + at; int t = rd16(q + MAT_TEXTURE0);
    if (!(rd32(q + MAT_FLAGS) & 3) || (t != c->base[0] && t != c->base[1])) return;
    int bottom = to == BOTTOM || to == TOP;
    wr16(q + MAT_TEXTURE0, (uint16_t)c->base[bottom]);
    wrf(pane + PANE_ROT_Z, to == LEFT || to == TOP ? 180.0f : 0.0f);
    wrf(pane + PANE_SCALE_X, to == TOP ? -1.0f : 1.0f); c->bases++;
}
static void layout_section(uint8_t *s, uint64_t size, const char *m, layout_ctx *c) {
    if (!strcmp(m, "txl1")) {
        static const char *const names[4] = {"Nt_KeyTexA_00", "Nt_KeyTexB_00", "Nt_KeyTexX_00", "Nt_KeyTexY_00"};
        if (size < 12) return;
        unsigned n = rd16(s + 8);
        for (unsigned i = 0; i < n && 12 + 4 * (uint64_t)i + 4 <= size; i++) {
            uint64_t at = 12 + (uint64_t)rd32(s + 12 + 4 * i); if (at >= size) continue;
            const char *t = (const char *)s + at; size_t room = (size_t)(size - at);
            for (int k = 0; k < 4; k++) { size_t len = strlen(names[k]); if (len <= room && !memcmp(t, names[k], len)) c->tex[k] = (int)i; }
            static const char *const bases[2] = {"Nt_KeyBtnSubBaseAL_00", "Nt_KeyBtnSubBaseBL_00"};
            for (int k = 0; k < 2; k++) { size_t len = strlen(bases[k]); if (len <= room && !memcmp(t, bases[k], len)) c->base[k] = (int)i; }
        }
        return;
    }
    if (!strcmp(m, "mat1")) { c->mat1 = s; c->mat1_size = size; return; }
    if (!strcmp(m, "pas1")) { if (c->depth < 16) snprintf(c->stack[c->depth++], 25, "%s", c->last); return; }
    if (!strcmp(m, "pae1")) { if (c->depth) c->depth--; return; }
    if (strcmp(m, "pan1") && strcmp(m, "pic1") && strcmp(m, "txt1") && strcmp(m, "wnd1") && strcmp(m, "prt1")) return;
    if (size < PANE_END) return;
    char name[PANE_NAME_LEN + 1]; memcpy(name, s + PANE_NAME, PANE_NAME_LEN); name[PANE_NAME_LEN] = 0; snprintf(c->last, 25, "%s", name);
    int g = pane_group(c); if (g < 0) return;
    int from = switch_place("ABXY"[g]), to = c->places[g], mir = mirror_group(c, g);
    const char *parent = c->depth ? c->stack[c->depth - 1] : "";
    int ring_parent = !strncmp(parent, "N_Loop", 6) || !strncmp(parent, "N_Stop", 6);
    if (!strcmp(m, "pic1") && (strstr(name, "Key") != NULL)) set_key_texture(c, s, size, g);   /* P_Key*, P_CaptureKey* */
    if (!strcmp(m, "pic1") && !strncmp(name, "P_Base", 6) && ring_parent) { set_backdrop(c, s, size, to); return; }
    if (ring_parent && (!strncmp(name, "N_Btn", 5) || !strncmp(name, "N_CompanionBtn", 14))) {   /* the key with its verb box */
        wrf(s + PANE_X, kKeyAt[to][0]); wrf(s + PANE_Y, kKeyAt[to][1]); c->moved++; return;
    }
    if (ring_parent && !strcmp(m, "txt1") && name[0] == 'T' && name[1] == '_' && name[3] == '_' && group_index(name[2]) >= 0) {   /* a ring letter */
        int at = switch_place(name[2]); if (at == to) at = from;
        wrf(s + PANE_X, kRingAt[at][0]); wrf(s + PANE_Y, kRingAt[at][1]); return;
    }
    if (!strncmp(name, "N_Rush", 6) || !strncmp(name, "N_Hold", 6)) {   /* effects beside the key: moved and mirrored as a whole */
        float sx = mir ? -1.0f : 1.0f;
        wrf(s + PANE_X, kKeyAt[to][0] - sx * kKeyAt[from][0]); wrf(s + PANE_Y, kKeyAt[to][1] - kKeyAt[from][1]);
        wrf(s + PANE_SCALE_X, sx * rdf(s + PANE_SCALE_X)); return;
    }
    if (mir && in_key_container(c)) {   /* the verb box on the other side of the key, as the Y diamond draws it */
        float x = rdf(s + PANE_X);
        if (!strcmp(m, "txt1")) {
            if (size < TXT_END) return;
            wrf(s + PANE_X, to == LEFT ? -x - 2 : -x + 2);
            uint8_t h = s[PANE_ORIGIN] & 3; if (h == 1 || h == 2) s[PANE_ORIGIN] = (uint8_t)((s[PANE_ORIGIN] & ~3) | (3 - h));   /* anchored on the other side */
            uint8_t a = s[TXT_ALIGN]; if (a == 1 || a == 2) s[TXT_ALIGN] = (uint8_t)(3 - a);                                     /* aligned to it */
        } else {
            wrf(s + PANE_X, -x);
            if (!strcmp(m, "wnd1")) wrf(s + PANE_SCALE_X, -rdf(s + PANE_SCALE_X));
        }
        c->mirrored++;
    }
}
static int edit_layout(uint8_t *f, uint64_t size, const int8_t places[4], const char letters[4]) {
    if (size < 0x14 || rd16(f + 4) != 0xfeff) return 0;
    layout_ctx c = {.tex = {-1, -1, -1, -1}, .base = {-1, -1}}; memcpy(c.places, places, 4); memcpy(c.letters, letters, 4);
    uint64_t p = rd16(f + 6); unsigned n = rd16(f + 0x10);
    for (unsigned i = 0; i < n && p + 8 <= size; i++) {   /* the texture list first: materials refer to it */
        uint64_t s = rd32(f + p + 4); if (s < 8 || p + s > size) return 0;
        if (!memcmp(f + p, "txl1", 4)) layout_section(f + p, s, "txl1", &c);
        p += s;
    }
    if (c.tex[0] < 0 || c.tex[1] < 0 || c.tex[2] < 0 || c.tex[3] < 0) return 0;
    p = rd16(f + 6);
    for (unsigned i = 0; i < n && p + 8 <= size; i++) {
        uint64_t s = rd32(f + p + 4); if (s < 8 || p + s > size) return 0;
        char m[5] = {0}; memcpy(m, f + p, 4); if (strcmp(m, "txl1")) layout_section(f + p, s, m, &c);
        p += s;
    }
    fprintf(stderr, "[totk-prompts] command bubble: %d keys moved, %d key pictures, %d backdrops, %d verb-box parts mirrored\n", c.moved, c.keys, c.bases, c.mirrored);
    return c.moved > 0;
}

/* ---- the messages ---- */
/* A message's text (UTF-16) by label in a little-endian MSBT, and its length in characters; NULL if none */
static uint16_t *msbt_text(uint8_t *b, uint64_t size, const char *label, unsigned *chars) {
    if (size < 0x20 || memcmp(b, "MsgStdBn", 8) || rd16(b + 8) != 0xfeff) return NULL;
    unsigned nsec = rd16(b + 0xe); uint64_t p = 0x20; int64_t index = -1; uint8_t *txt = NULL; uint64_t txt_size = 0; size_t want = strlen(label);
    for (unsigned k = 0; k < nsec && p + 0x10 <= size; k++) {
        uint64_t s = rd32(b + p + 4); uint8_t *body = b + p + 0x10;
        if (p + 0x10 + s > size) return NULL;
        if (!memcmp(b + p, "LBL1", 4) && s >= 4) {   /* groups of {count, offset}, each a run of {length, name, index} */
            uint64_t groups = rd32(body);
            for (uint64_t i = 0; i < groups && index < 0 && 4 + 8 * i + 8 <= s; i++) {
                uint64_t cnt = rd32(body + 4 + 8 * i), q = rd32(body + 8 + 8 * i);
                for (uint64_t j = 0; j < cnt && q < s; j++) { uint64_t len = body[q];
                    if (q + 1 + len + 4 > s) break;
                    if (len == want && !memcmp(body + q + 1, label, len)) { index = rd32(body + q + 1 + len); break; }
                    q += 1 + len + 4; }
            }
        }
        if (!memcmp(b + p, "TXT2", 4)) { txt = body; txt_size = s; }
        p += 0x10 + s; p = (p + 15) & ~15ull;
    }
    if (index < 0 || !txt || txt_size < 4) return NULL;
    uint64_t count = rd32(txt); if ((uint64_t)index >= count || 4 + 4 * count > txt_size) return NULL;
    uint64_t a = rd32(txt + 4 + 4 * index), e = (uint64_t)index + 1 < count ? rd32(txt + 8 + 4 * index) : txt_size;
    if (a > e || e > txt_size || (a & 1)) return NULL;
    *chars = (unsigned)((e - a) / 2); return (uint16_t *)(void *)(txt + a);
}
static const struct { const char *pane; char group; } kRing[] = {
    {"T_B_00", 'A'}, {"T_X_00", 'A'}, {"T_Y_00", 'A'}, {"T_B_01", 'A'}, {"T_X_02", 'A'}, {"T_Y_03", 'A'},
    {"T_A_01", 'B'}, {"T_X_01", 'B'}, {"T_Y_01", 'B'}, {"T_A_02", 'X'}, {"T_B_02", 'X'}, {"T_Y_02", 'X'},
    {"T_A_03", 'Y'}, {"T_B_03", 'Y'}, {"T_X_03", 'Y'}};
static int edit_messages(uint8_t *sarc, uint64_t n, const int8_t places[4], const char letters[4], uint16_t ability_char) {
    int changed = 0; uint64_t mn; uint8_t *msg = sarc_member(sarc, n, "LayoutMsg/DoCommand_00.msbt", &mn);
    if (msg && places[0] >= 0)
        for (unsigned i = 0; i < sizeof kRing / sizeof *kRing; i++) {
            unsigned len; uint16_t *t = msbt_text(msg, mn, kRing[i].pane, &len); if (!t || len < 1) continue;
            int g = group_index(kRing[i].group), at = switch_place(kRing[i].pane[2]); if (at == places[g]) at = switch_place(kRing[i].group);
            uint16_t letter = (uint16_t)letters[at];
            /* the symbols by position, as the style's icons: the text fonts have these, the icon font is out of this pane's reach */
            if (dm_android_prompt_glyphs()) { static const uint16_t symbol[4] = {0x25CB, 0xD7, 0x25B3, 0x25A1}; letter = symbol[group_index(letters[at])]; }
            if (t[0] != letter) { wr16((uint8_t *)t, letter); changed++; }
        }
    msg = sarc_member(sarc, n, "LayoutMsg/PlayerStatus_00.msbt", &mn);
    if (msg && ability_char) { unsigned len; uint16_t *t = msbt_text(msg, mn, "Pa_ShortCutGuide_00-T_Icon_00", &len);
        if (t && len >= 1 && t[0] != ability_char) { wr16((uint8_t *)t, ability_char); changed++; } }
    return changed;
}

/* ---- the quick menus' capture ----
   The weapon, shield and material menus and the ability wheel capture the game picture for their background when the panes
   below turn visible, at frame 1.0 of their opening animation. UI animations advance half a frame per game frame at 60 fps,
   so the capture lands after the world has stopped drawing and the background can come out black. The visibility key moves
   to frame 0.0 and the live world is captured at once. The same edit as MacNativeGames scripts/make_romfs_patches.py
   (patch_capture): only a pane with the shipped keys (0.0, hidden) (1.0, shown) moves, so a mod's own animation stays as it
   is, and a file that was already edited changes nothing.
   BFLAN: sections from FLAN_FIRST; pai1 holds an offset table of panes, each with its name, a table of tags, each with a
   table of tracks. All offsets count from the start of what holds them. A visibility tag is FLVI, its tracks step curves. */
enum {
    FLAN_SECTIONS = 0x10, FLAN_FIRST = 0x14,
    PAI_PANES = 0x0e, PAI_TABLE = 0x10, PAI_END = 0x14,
    PANE_ANIM_NAME_LEN = 28, PANE_ANIM_TAGS = 0x1c, PANE_ANIM_TABLE = 0x20,
    TAG_TRACKS = 0x04, TAG_TABLE = 0x08,
    TRACK_CURVE = 0x02, TRACK_KEYS = 0x04, TRACK_KEY_TABLE = 0x08, TRACK_END = 0x0c,
    CURVE_STEP = 1, KEY_SIZE = 8, KEY_VALUE = 4, CAPTURE_MOST = 8,
};
static const struct { const char *member; const char *pane[3]; } kCapture[] = {
    {"anim/ShortCutBG_00_In.bflan", {"C_CaptureBG_00", "C_CaptureClearBG_00", "P_CaptureClearBG_00"}},
    {"anim/AutoBuilder_00_In.bflan", {"C_CaptureBG_00", NULL, NULL}},
};
/* The u32 offset at f + at, counted from `from`; 0 if it leaves [0, size) */
static uint64_t flan_offset(const uint8_t *f, uint64_t size, uint64_t at, uint64_t from) {
    if (at > size || size - at < 4) return 0;
    uint64_t to = from + rd32(f + at);
    return to < size ? to : 0;
}
/* A pane's visibility tracks: the second key of each with the shipped keys goes into second[]. -1: the file does not hold together */
static int capture_pane(const uint8_t *f, uint64_t size, uint64_t pane, uint64_t second[CAPTURE_MOST], int *found, int *other) {
    if (size - pane < PANE_ANIM_TABLE) return -1;
    unsigned tags = f[pane + PANE_ANIM_TAGS];
    for (unsigned t = 0; t < tags; t++) {
        uint64_t tag = flan_offset(f, size, pane + PANE_ANIM_TABLE + 4ull * t, pane);
        if (!tag || size - tag < TAG_TABLE) return -1;
        if (memcmp(f + tag, "FLVI", 4)) continue;
        unsigned tracks = f[tag + TAG_TRACKS];
        for (unsigned g = 0; g < tracks; g++) {
            uint64_t track = flan_offset(f, size, tag + TAG_TABLE + 4ull * g, tag);
            if (!track || size - track < TRACK_END) return -1;
            unsigned n = rd16(f + track + TRACK_KEYS);
            uint64_t keys = flan_offset(f, size, track + TRACK_KEY_TABLE, track);
            if (!keys || size - keys < (uint64_t)n * KEY_SIZE) return -1;
            if (f[track + TRACK_CURVE] == CURVE_STEP && n == 2 && *found < CAPTURE_MOST && rdf(f + keys) == 0.0f && !rd16(f + keys + KEY_VALUE) &&
                rdf(f + keys + KEY_SIZE) == 1.0f && rd16(f + keys + KEY_SIZE + KEY_VALUE) == 1) second[(*found)++] = keys + KEY_SIZE;
            else (*other)++;
        }
    }
    return 0;
}
/* The panes of one animation; how many moved. Nothing changes unless the whole file holds together. */
static int capture_member(uint8_t *f, uint64_t size, const char *const *names, int *other) {
    if (size < FLAN_FIRST || memcmp(f, "FLAN", 4) || rd16(f + 4) != 0xfeff) return 0;
    uint64_t second[CAPTURE_MOST], pos = FLAN_FIRST; int found = 0; unsigned sections = rd16(f + FLAN_SECTIONS);
    for (unsigned s = 0; s < sections && size - pos >= 8; s++) {
        uint64_t len = rd32(f + pos + 4); if (len < 8 || len > size - pos) return 0;
        if (!memcmp(f + pos, "pai1", 4)) {
            if (len < PAI_END) return 0;
            unsigned panes = rd16(f + pos + PAI_PANES); uint64_t table = flan_offset(f, size, pos + PAI_TABLE, pos);
            if (!table && panes) return 0;
            for (unsigned e = 0; e < panes; e++) {
                uint64_t pane = flan_offset(f, size, table + 4ull * e, pos); if (!pane || size - pane < PANE_ANIM_NAME_LEN) return 0;
                char name[PANE_ANIM_NAME_LEN + 1]; memcpy(name, f + pane, PANE_ANIM_NAME_LEN); name[PANE_ANIM_NAME_LEN] = 0;
                for (int k = 0; k < 3 && names[k]; k++)
                    if (!strcmp(name, names[k]) && capture_pane(f, size, pane, second, &found, other)) return 0;
            }
        }
        pos += len;
    }
    for (int i = 0; i < found; i++) wrf(f + second[i], 0.0f);
    return found;
}
static int capture_timing(uint8_t *sarc, uint64_t n) {
    int moved = 0, other = 0;
    for (unsigned i = 0; i < sizeof kCapture / sizeof *kCapture; i++) {
        uint64_t mn; uint8_t *member = sarc_member(sarc, n, kCapture[i].member, &mn);
        if (member) moved += capture_member(member, mn, kCapture[i].pane, &other);
    }
    fprintf(stderr, "[totk-capture] quick menus: %d capture keys moved to frame 0, %d left as they were\n", moved, other);
    return moved;
}

/* ---- the heap sizes ----
   A render resolution above 1600x900 (supersampling, TOTK_RENDER_RES) needs bigger render-target heaps: the game's
   DynamicTextureAllocatorMem (113664 KiB) and its parent Graphics (201728 KiB) in System/HeapSize/Product.Nin_NX_NVN.
   HeapDef.byml.zs, both grown by runtime_output_res_extra_bytes() (the runtime adds the same to the application pool).
   The Mac builds this file once with scripts/make_output_res_overlay.py; here it is made as the game opens it, the same
   two numbers changed in place (BYML v7: hash nodes 0xC1 of {u24 key index, u8 type, u32 value}, ints 0xD1 inline). From the
   player's own files no overlay can be there, so a table with other sizes is a mod's and gets the same growth on top of its
   own (the Mac's --heapdef); otherwise it may be the PC's overlay, grown already, and stays. */
static const char kHeapDef[] = "System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs";
/* The value offset of hash node `node`'s entry `name` of type `type`, or 0 */
static uint64_t byml_field(const uint8_t *b, uint64_t n, uint64_t keys, uint64_t node, const char *name, uint8_t type) {
    if (node + 4 > n || keys + 4 > n || b[node] != 0xc1) return 0;
    uint32_t count = rd32(b + node) >> 8, nkeys = rd32(b + keys) >> 8;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t e = node + 4 + 8ull * i; if (e + 8 > n) return 0;
        uint32_t k = rd32(b + e) & 0xffffff; if (k >= nkeys || keys + 8 + 4ull * k > n) return 0;
        uint64_t s = keys + rd32(b + keys + 4 + 4ull * k), len = strlen(name) + 1;
        if (s + len <= n && !memcmp(b + s, name, len)) return b[e + 3] == type ? e + 4 : 0;
    }
    return 0;
}
static int heap_sizes(const uint8_t *data, uint64_t size, uint8_t **out, uint64_t *out_size) {
    uint64_t extra = runtime_output_res_extra_bytes() >> 10;
    unsigned long long m = ZSTD_getFrameContentSize(data, size);
    if (!extra || ZSTD_getDictID_fromFrame(data, size) || m == ZSTD_CONTENTSIZE_UNKNOWN || m == ZSTD_CONTENTSIZE_ERROR || m < 16 || m > (1u << 20)) return 0;
    uint64_t skip = RAW_FRAME_HEAD + RAW_BLOCK_HEAD * raw_blocks(m);
    uint8_t *frame = malloc(skip + m), *b = frame + skip; if (!frame) return 0;
    if (ZSTD_decompress(b, m, data, size) != m || memcmp(b, "YB", 2) || rd16(b + 2) != 7) { free(frame); return 0; }
    uint64_t keys = rd32(b + 4), root = rd32(b + 12), heaps = byml_field(b, m, keys, root, "Heaps", 0xc1), at[2] = {0, 0};
    static const char *const kHeaps[2] = {"DynamicTextureAllocatorMem", "Graphics"}; static const uint32_t kShipped[2] = {113664, 201728};
    uint32_t was[2] = {0, 0}; int own_files = player_on && player_on();
    for (int i = 0; i < 2 && heaps; i++) {
        uint64_t h = byml_field(b, m, keys, rd32(b + heaps), kHeaps[i], 0xc1);
        at[i] = h ? byml_field(b, m, keys, rd32(b + h), "Size", 0xd1) : 0;
        if (at[i]) was[i] = rd32(b + at[i]);
        if (!at[i] || (was[i] != kShipped[i] && !own_files)) at[i] = 0;
    }
    if (!at[0] || !at[1]) { fprintf(stderr, "[totk-heaps] %s is not the shipped one: unchanged\n", kHeapDef); free(frame); return 0; }
    for (int i = 0; i < 2; i++) { uint32_t v = was[i] + (uint32_t)extra; memcpy(b + at[i], &v, 4); }
    fprintf(stderr, "[totk-heaps] render-target heaps +%llu KiB (DynamicTextureAllocatorMem %u, Graphics %u KiB)\n",
            (unsigned long long)extra, was[0] + (unsigned)extra, was[1] + (unsigned)extra);
    *out = frame; *out_size = finish_raw_frame(frame, m); return 1;
}

static int generate(const char *path, const uint8_t *data, uint64_t size, uint8_t **out, uint64_t *out_size) {
    if (!strcmp(path, kHeapDef)) return !runtime_output_res_extra_bytes() ? 0 : !data ? 1 : heap_sizes(data, size, out, out_size);
    int layout = !strcmp(path, "UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs");
    size_t pl = strlen(path); int messages = !strncmp(path, "Mals/", 5) && pl > 8 && !strcmp(path + pl - 8, ".sarc.zs");
    if (!layout && !messages) return 0;
    int8_t places[4]; uint16_t ability = 0; char letters[4];
    int prompts = dm_android_prompt_places(places, &ability, letters);
    if (!prompts && !layout) return 0;   /* the layout still has the capture timing */
    if (!data) return 1;
    uint8_t *sarc; uint64_t n; uint8_t *frame = open_archive(data, size, &sarc, &n);
    if (!frame) { fprintf(stderr, "[totk-prompts] %s did not decompress: unchanged\n", path); return 0; }
    int changed = 0;
    if (layout) {
        uint64_t ln; uint8_t *lay = sarc_member(sarc, n, "blyt/DoCommand_00.bflyt", &ln);
        changed = capture_timing(sarc, n);
        if (prompts && lay && places[0] >= 0 && edit_layout(lay, ln, places, letters)) changed = 1;
    } else { changed = edit_messages(sarc, n, places, letters, ability); fprintf(stderr, "[totk-prompts] %s: %d prompt letters\n", path, changed); }
    if (!changed) { free(frame); return 0; }
    *out = frame; *out_size = finish_raw_frame(frame, n); return 1;
}
__attribute__((constructor)) static void prompts_romfs_init(void) { runtime_romfs_generator = generate; }

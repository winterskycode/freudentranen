/* The pair of files: which is which and whether they are the right ones, then the sections of the update and the base that
   hold the game. */
#include "container_internal.h"
#include <stdlib.h>

#define MAX_MODULE (256u << 20)
#define MAX_HASH_BLOCK (16u << 20)

static const char *const module_names[TOKC_MODULE_COUNT] = {"main", "sdk", "subsdk0"};

typedef unsigned long long ull;

static const tokc_meta *find_meta(const tokc_package *p, uint64_t title, uint8_t type) {
    for (uint32_t i = 0; i < p->nmetas; i++) if (p->metas[i].title_id == title && p->metas[i].type == type) return &p->metas[i];
    return NULL;
}

static int check_base(const tokc_package *p, char *err, size_t errlen) {
    const tokc_meta *app = find_meta(p, TOKC_TITLE_BASE, TOKC_META_APPLICATION);
    if (!app) {
        if (find_meta(p, TOKC_TITLE_UPDATE, TOKC_META_PATCH)) return tokc_fail(err, errlen, TOKC_E_SWAPPED, "the file given as the base game is the update");
        return tokc_fail(err, errlen, TOKC_E_WRONG_GAME, "the file given as the base game is not Tears of the Kingdom (title %016llx)", (ull)p->metas[0].title_id);
    }
    if (app->version != TOKC_VERSION_BASE) return tokc_fail(err, errlen, TOKC_E_WRONG_VERSION, "the base game is not 1.0.0 (its version number is %u)", app->version);
    return TOKC_OK;
}

static int check_update(const tokc_package *p, char *err, size_t errlen) {
    const tokc_meta *patch = find_meta(p, TOKC_TITLE_UPDATE, TOKC_META_PATCH);
    if (!patch) {
        if (find_meta(p, TOKC_TITLE_BASE, TOKC_META_APPLICATION)) return tokc_fail(err, errlen, TOKC_E_SWAPPED, "the file given as the update is the base game");
        return tokc_fail(err, errlen, TOKC_E_WRONG_GAME, "the file given as the update is not for Tears of the Kingdom (title %016llx)", (ull)p->metas[0].title_id);
    }
    if (patch->version != TOKC_VERSION_UPDATE) return tokc_fail(err, errlen, TOKC_E_WRONG_VERSION, "the update is not 1.4.3 (its version number is %u, 1.4.3 has %u)", patch->version, TOKC_VERSION_UPDATE);
    return TOKC_OK;
}

/* The game's Program NCA; a file with two holds more than one version, which is not supported. */
static const tokc_nca *find_program(const tokc_package *p, const char *what, char *err, size_t errlen) {
    const tokc_nca *found = NULL;
    for (uint32_t i = 0; i < p->nncas; i++) {
        const tokc_nca *n = &p->ncas[i];
        if (n->content_type || n->title_id != TOKC_TITLE_BASE) continue;
        if (found) { tokc_fail(err, errlen, TOKC_E_FORMAT, "the %s file holds more than one program; use separate base and update files", what); return NULL; }
        found = n;
    }
    if (!found) tokc_fail(err, errlen, TOKC_E_FORMAT, "the %s file has no program", what);
    return found;
}

/* The section of the kind asked for, -1 if none. */
static int find_section(const tokc_nca *n, int fs_type, int hash_type, int crypt) {
    for (int i = 0; i < 4; i++) {
        if (!tokc_section_present(n, i)) continue;
        const uint8_t *fs = NCA_FS(n, i);
        if (fs[2] == fs_type && fs[3] == hash_type && fs[4] == crypt) return i;
    }
    return -1;
}

/* The data level of a RomFS section's IVFC header. */
static int romfs_level(const uint8_t *fs, uint64_t section_size, uint64_t *off, uint64_t *size, char *err, size_t errlen) {
    const uint8_t *ivfc = fs + 8;
    if (memcmp(ivfc, "IVFC", 4) || rd32(ivfc + 0xC) != 7) return tokc_fail(err, errlen, TOKC_E_FORMAT, "a RomFS section has no IVFC header");
    *off = rd64(ivfc + 0x10 + 5 * 0x18); *size = rd64(ivfc + 0x10 + 5 * 0x18 + 8);
    if (*size < 0x50 || !fits(*off, *size, section_size)) return tokc_fail(err, errlen, TOKC_E_FORMAT, "a RomFS section's data is out of bounds");
    return TOKC_OK;
}

static int open_exefs(tokc_game *g, const tokc_nca *program, const tokc_keys *keys, const tokc_package *const *pk, char *err, size_t errlen) {
    int i = find_section(program, FS_PFS0, HASH_SHA256, CRYPT_CTR);
    if (i < 0) return tokc_fail(err, errlen, TOKC_E_FORMAT, "the update has no ExeFS");
    int rc = tokc_section_open(&g->exefs, program, i, keys, pk, 2, err, errlen);
    if (rc) return rc;
    const uint8_t *fs = NCA_FS(program, i);
    memcpy(g->master_hash, fs + 8, 32);
    g->hash_block = rd32(fs + 0x28);
    g->hash_off = rd64(fs + 0x30); g->hash_size = rd64(fs + 0x38);
    g->pfs_off = rd64(fs + 0x40); g->pfs_size = rd64(fs + 0x48);
    uint64_t size = g->exefs.size;
    if (!fits(g->pfs_off, g->pfs_size, size) || !fits(g->hash_off, g->hash_size, size) || !g->hash_block || g->hash_block > MAX_HASH_BLOCK ||
        g->hash_size / 32 < (g->pfs_size + g->hash_block - 1) / g->hash_block || g->hash_size > MAX_MODULE)
        return tokc_fail(err, errlen, TOKC_E_FORMAT, "the update's ExeFS header is out of bounds");
    uint8_t magic[4];
    if ((rc = tokc_section_read(&g->exefs, magic, 4, g->pfs_off))) return tokc_fail(err, errlen, rc, "cannot read the update's ExeFS");
    if (memcmp(magic, "PFS0", 4)) return tokc_fail(err, errlen, TOKC_E_KEY_WRONG, "the keys do not decrypt the update's program: prod.keys or the ticket does not match this file");
    if ((rc = tokc_part_parse(tokc_section_read, &g->exefs, g->pfs_off, g->pfs_size, "PFS0", 24, &g->modules, &g->nmodules, err, errlen))) return rc;
    for (int m = 0; m < TOKC_MODULE_COUNT; m++) {
        uint32_t k = 0;
        while (k < g->nmodules && strcmp(g->modules[k].name, module_names[m])) k++;
        if (k == g->nmodules || !g->modules[k].size || g->modules[k].size > MAX_MODULE)
            return tokc_fail(err, errlen, TOKC_E_FORMAT, "the update's ExeFS has no usable %s", module_names[m]);
    }
    return TOKC_OK;
}

static int open_romfs(tokc_game *g, const tokc_nca *base, const tokc_nca *update, const tokc_keys *keys, const tokc_package *const *pk, char *err, size_t errlen) {
    int bi = find_section(base, FS_ROMFS, HASH_IVFC, CRYPT_CTR), ui = find_section(update, FS_ROMFS, HASH_IVFC, CRYPT_CTR_EX);
    if (bi < 0) return tokc_fail(err, errlen, TOKC_E_FORMAT, "the base game has no RomFS");
    if (ui < 0) return tokc_fail(err, errlen, TOKC_E_FORMAT, "the update has no patched RomFS");
    int rc = tokc_section_open(&g->base, base, bi, keys, pk, 2, err, errlen);
    if (!rc) rc = tokc_section_open(&g->update, update, ui, keys, pk, 2, err, errlen);
    if (rc) return rc;
    uint64_t off, size;
    uint8_t h[8];
    if ((rc = romfs_level(NCA_FS(base, bi), g->base.size, &off, &size, err, errlen))) return rc;
    if ((rc = tokc_section_read(&g->base, h, 8, off))) return tokc_fail(err, errlen, rc, "cannot read the base game's RomFS");
    if (rd64(h) != 0x50) return tokc_fail(err, errlen, TOKC_E_KEY_WRONG, "the keys do not decrypt the base game's RomFS: prod.keys or the ticket does not match this file");
    g->base_image_off = off; g->base_image_size = size;
    if ((rc = tokc_bktr_load(&g->bktr, &g->update, NCA_FS(update, ui), g->base.size, err, errlen))) return rc;
    if ((rc = romfs_level(NCA_FS(update, ui), g->bktr.total, &g->image_off, &g->image_size, err, errlen))) return rc;
    int64_t got = tokc_romfs_pread(g, h, 8, 0);
    if (got != 8) return tokc_fail(err, errlen, got < 0 ? (int)got : TOKC_E_IO, "cannot read the patched RomFS");
    if (rd64(h) != 0x50) return tokc_fail(err, errlen, TOKC_E_KEY_WRONG, "the patched RomFS does not decrypt: the keys or the base game do not match the update");
    return TOKC_OK;
}

int tokc_game_open(int base_fd, int update_fd, const tokc_keys *keys, tokc_game **out, char *err, size_t errlen) {
    if (!keys || !out) return tokc_fail(err, errlen, TOKC_E_ARGS, "no keys");
    *out = NULL;
    tokc_package base, update;
    int rc = tokc_package_open(&base, base_fd, keys, err, errlen);
    if (rc) return rc;
    if ((rc = check_base(&base, err, errlen))) { tokc_package_free(&base); return rc; }
    if ((rc = tokc_package_open(&update, update_fd, keys, err, errlen))) { tokc_package_free(&base); return rc; }
    tokc_game *g = NULL;
    if ((rc = check_update(&update, err, errlen))) goto out;
    const tokc_nca *bp = find_program(&base, "base game", err, errlen), *up = find_program(&update, "update", err, errlen);
    if (!bp || !up) { rc = TOKC_E_FORMAT; goto out; }
    g = calloc(1, sizeof *g);
    if (!g) { rc = tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory"); goto out; }
    const tokc_package *const pk[2] = {&base, &update};
    if (!(rc = open_exefs(g, up, keys, pk, err, errlen))) rc = open_romfs(g, bp, up, keys, pk, err, errlen);
    if (!rc) { *out = g; g = NULL; }
out:
    tokc_game_close(g);
    tokc_package_free(&base); tokc_package_free(&update);
    return rc;
}

void tokc_game_close(tokc_game *g) {
    if (!g) return;
    tokc_bktr_free(&g->bktr);
    free(g->modules);
    tokc_wipe(g, sizeof *g);
    free(g);
}

int tokc_game_module(const tokc_game *g, tokc_module m, uint8_t **bytes, size_t *size, char *err, size_t errlen) {
    if (!g || !bytes || !size || (unsigned)m >= TOKC_MODULE_COUNT) return tokc_fail(err, errlen, TOKC_E_ARGS, "no such module");
    *bytes = NULL; *size = 0;
    const tokc_entry *e = g->modules;
    while (strcmp(e->name, module_names[m])) e++;   /* open_exefs saw it */
    uint8_t *table = malloc((size_t)g->hash_size), *block = malloc(g->hash_block), *data = malloc((size_t)e->size), hash[32];
    int rc = TOKC_OK;
    if (!table || !block || !data) { rc = tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory"); goto out; }
    if ((rc = tokc_section_read(&g->exefs, table, (size_t)g->hash_size, g->hash_off))) { rc = tokc_fail(err, errlen, rc, "cannot read the ExeFS hashes"); goto out; }
    tokc_sha256_buf(table, (size_t)g->hash_size, hash);
    if (memcmp(hash, g->master_hash, 32)) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "the ExeFS hash table fails its hash: the update file is damaged"); goto out; }
    uint64_t start = e->off - g->pfs_off, end = start + e->size;
    for (uint64_t b = start / g->hash_block; b * g->hash_block < end; b++) {
        uint64_t from = b * g->hash_block, n = g->pfs_size - from < g->hash_block ? g->pfs_size - from : g->hash_block;
        if ((rc = tokc_section_read(&g->exefs, block, (size_t)n, g->pfs_off + from))) { rc = tokc_fail(err, errlen, rc, "cannot read the ExeFS"); goto out; }
        tokc_sha256_buf(block, (size_t)n, hash);
        if (memcmp(hash, table + 32 * b, 32)) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "%s fails its hash: the update file is damaged", module_names[m]); goto out; }
        uint64_t a = from > start ? from : start, z = from + n < end ? from + n : end;
        memcpy(data + (a - start), block + (a - from), (size_t)(z - a));
    }
    *bytes = data; *size = (size_t)e->size; data = NULL;
out:
    free(table); free(block); free(data);
    return rc;
}

uint64_t tokc_romfs_size(const tokc_game *g) { return g ? g->image_size : 0; }
uint64_t tokc_base_romfs_size(const tokc_game *g) { return g ? g->base_image_size : 0; }

void tokc_game_stats(const tokc_game *g, tokc_stats *out) {
    const tokc_bktr *b = &g->bktr;
    memset(out, 0, sizeof *out);
    out->relocations = b->nrel; out->subsections = b->nsub;
    out->table_bytes = b->nrel * 16 + b->nsub * 12;
    out->image_offset = g->image_off;
    for (uint64_t i = 0; i < b->nrel; i++) {
        uint64_t len = (i + 1 < b->nrel ? b->virt[i + 1] : b->total) - b->virt[i];
        if (b->phys[i] & TOKC_BKTR_PATCH) out->update_bytes += len;
        else out->base_bytes += len;
    }
}

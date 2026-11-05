/* TOTK_VK_TEXPROBE=1: could texture descriptors live in one global array indexed by the game's texture id (bindless)?
   For each (id, view type) a draw resolves, the view it got and the last batch that used it. A later draw resolving
   another view for the same key would have to overwrite that array entry: fine once the batches that used the old one
   are done, not while one is still in flight (or is the batch being recorded). Changes are split into the game giving
   the id another texture or view (re-registration) and our own view changing for the same texture. Logged every 300
   presents as [texprobe], with the first conflicts in detail. */
typedef struct { uint32_t key; VkImageView view; uint64_t tex_serial, batch; view_t v; } texprobe_entry;
static texprobe_entry *texprobe_table; static uint32_t texprobe_mask;
static int texprobe_state = -1;
static uint64_t texprobe_uses, texprobe_new, texprobe_changed_game, texprobe_changed_ours, texprobe_conflict_game, texprobe_conflict_ours,
                texprobe_same_batch, texprobe_last;
static int texprobe_on_slow(void);
static inline int texprobe_on(void) { return texprobe_state >= 0 ? texprobe_state : texprobe_on_slow(); }
static int texprobe_on_slow(void) {
    if (texprobe_state < 0) { const char *e = getenv("TOTK_VK_TEXPROBE"); texprobe_state = e && atoi(e) > 0;
        if (texprobe_state) { texprobe_mask = (1u << 20) - 1; texprobe_table = calloc(texprobe_mask + 1, sizeof *texprobe_table); if (!texprobe_table) die("texprobe");
            LOG("[texprobe] on: per texture id and view type, view changes and whether a batch using the old view is in flight"); } }
    return texprobe_state;
}
/* bind_shader, translation thread: id = the handle's texture index, type = the binding's NVN sampler type */
static void texprobe_use(uint32_t id, uint32_t type, const texture_descriptor *d, VkImageView view) {
    /* + 1: 0 marks an empty slot */
    uint32_t key = (id << 4 | ((type & 255) + (type & 256 ? 8 : 0))) + 1;
    uint32_t i = (key * 0x9E3779B1u) >> 12 & texprobe_mask;
    while (texprobe_table[i].key && texprobe_table[i].key != key) i = (i + 1) & texprobe_mask;
    texprobe_entry *e = &texprobe_table[i]; uint64_t batch = post_batch_serial; texprobe_uses++;
    uint64_t tex = d->valid ? d->texture.serial : 0; view_t v = d->valid ? d->view : (view_t){0};
    if (!e->key) { *e = (texprobe_entry){key, view, tex, batch, v}; texprobe_new++; }
    else if (e->view != view) {
        int game = e->tex_serial != tex || memcmp(&e->v, &v, sizeof v);
        /* batches before the oldest pending one are done */
        uint64_t oldest = async_first ? async_first->serial : batch;
        int in_flight = e->batch >= oldest;
        if (game) texprobe_changed_game++; else texprobe_changed_ours++;
        if (in_flight) { if (game) texprobe_conflict_game++; else texprobe_conflict_ours++; if (e->batch == batch) texprobe_same_batch++;
            static unsigned shown; if (shown < 60) { shown++;
                LOG("[texprobe] conflict: frame %llu id %u type %#x %s: texture serial %llu -> %llu, last used by batch %llu, now batch %llu (oldest pending %llu)",
                    (unsigned long long)presents, id, type, game ? "the game changed it" : "our view changed", (unsigned long long)e->tex_serial, (unsigned long long)tex,
                    (unsigned long long)e->batch, (unsigned long long)batch, (unsigned long long)oldest); } }
        e->view = view; e->tex_serial = tex; e->v = v;
    }
    e->batch = batch;
    if (presents >= texprobe_last + 300) {
        if (texprobe_last) { double f = (double)(presents - texprobe_last);
            LOG("[texprobe] frame=%llu per frame: %.0f texture uses, %.1f new keys; view changes: %.2f by the game, %.2f ours; in flight: %.2f by the game, %.2f ours (%.2f within one batch)",
                (unsigned long long)presents, texprobe_uses / f, texprobe_new / f, texprobe_changed_game / f, texprobe_changed_ours / f,
                texprobe_conflict_game / f, texprobe_conflict_ours / f, texprobe_same_batch / f); }
        texprobe_last = presents; texprobe_uses = texprobe_new = texprobe_changed_game = texprobe_changed_ours = texprobe_conflict_game = texprobe_conflict_ours = texprobe_same_batch = 0;
    }
}

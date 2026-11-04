/* TOTK_VK_UBOPROBE=1: would the GPU read the right uniform bytes straight from game memory (no arena copy)? Every
   distinct uniform block version a batch uses is noted with a hash of the bytes the batch was given; when the batch's
   fence has signalled (the GPU has read them), the completion thread reads the game's memory there again. A block whose
   bytes changed in between is one the game rewrote before its GPU work finished: reading it in place would have been
   wrong. Logged every 300 presents as [uboprobe], with the ranges that changed most. */
enum { UBOPROBE_MEMORY, UBOPROBE_CAPTURE, UBOPROBE_KNOWN_COPY, UBOPROBE_COPY_OVERLAY, UBOPROBE_GPU_PENDING, UBOPROBE_CLASSES };
static const char *const uboprobe_class_name[UBOPROBE_CLASSES] = {"game memory", "submit capture", "known copy", "memory + pending copies", "pending GPU write"};
typedef struct { uintptr_t address; uint32_t size, cls; uint64_t hash; uint8_t *copy; } uboprobe_entry;   /* copy: capture-class bytes as given */
static struct { uboprobe_entry *e; uint32_t n, cap; } uboprobe_slots[DM_VK_BATCH_SLOTS];
static int uboprobe_state = -1;
static int uboprobe_on(void) {
    if (uboprobe_state < 0) { const char *e = getenv("TOTK_VK_UBOPROBE"); uboprobe_state = e && atoi(e) > 0;
        if (uboprobe_state) LOG("[uboprobe] on: uniform bytes given to the GPU against game memory at batch completion"); }
    return uboprobe_state;
}
static uint64_t uboprobe_hash(const uint8_t *p, uint32_t n) {
    uint64_t h = 14695981039346656037ull; for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull; return h;
}
/* uniform_read_cached, translation thread: the version just served for [address, address+size), bytes as given */
static void uboprobe_note(uintptr_t address, uint32_t size, const uint8_t *bytes, int cls) {
    if (!async_current) return;
    unsigned slot = (unsigned)(async_current - async_batches);
    typeof(uboprobe_slots[0]) *s = &uboprobe_slots[slot]; uint64_t h = uboprobe_hash(bytes, size);
    for (uint32_t i = s->n; i-- > 0 && i + 64 >= s->n;) if (s->e[i].address == address && s->e[i].size == size && s->e[i].hash == h) return;   /* noted just now */
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 1024; s->e = realloc(s->e, s->cap * sizeof *s->e); if (!s->e) die("uboprobe"); }
    s->e[s->n++] = (uboprobe_entry){address, size, (uint32_t)cls, h, NULL};
    if (cls == UBOPROBE_CAPTURE && (s->e[s->n - 1].copy = malloc(size))) memcpy(s->e[s->n - 1].copy, bytes, size);
}
typedef struct { uintptr_t address; uint32_t size, cls; uint64_t changed; } uboprobe_top;
static uboprobe_top uboprobe_tops[32]; static unsigned uboprobe_ntops;
static uint64_t uboprobe_checked, uboprobe_changed, uboprobe_changed_bytes, uboprobe_unreadable, uboprobe_last, uboprobe_cls_checked[UBOPROBE_CLASSES], uboprobe_cls_changed[UBOPROBE_CLASSES];
/* async_completion_main, holding queue_lock, after the batch's fence */
static void uboprobe_batch_done(unsigned slot) {
    typeof(uboprobe_slots[0]) *s = &uboprobe_slots[slot]; static uint8_t buf[65536];
    for (uint32_t i = 0; i < s->n; i++) { const uboprobe_entry *e = &s->e[i]; uboprobe_checked++; uboprobe_cls_checked[e->cls]++;
        if (dm_android_read_memory(e->address, buf, e->size)) { uboprobe_unreadable++; continue; }
        if (uboprobe_hash(buf, e->size) == e->hash) continue;
        uboprobe_changed++; uboprobe_changed_bytes += e->size; uboprobe_cls_changed[e->cls]++;
        if (e->cls == UBOPROBE_CAPTURE && e->copy) { static uint64_t window; static unsigned shown; if (presents / 300 != window) { window = presents / 300; shown = 0; }
            if (shown < 12) { shown++; uint32_t first = 0, last = 0, n = 0;
                for (uint32_t b = 0; b < e->size; b++) if (e->copy[b] != buf[b]) { if (!n) first = b; last = b; n++; }
                float was[4] = {0}, now[4] = {0}; uint32_t a = first & ~15u; if (a + 16 <= e->size) { memcpy(was, e->copy + a, 16); memcpy(now, buf + a, 16); }
                LOG("[uboprobe] capture changed: frame %llu %#llx+%u: %u bytes differ in %u..%u; at %u was %g %g %g %g now %g %g %g %g", (unsigned long long)presents,
                    (unsigned long long)e->address, e->size, n, first, last, a, was[0], was[1], was[2], was[3], now[0], now[1], now[2], now[3]); } }
        unsigned k = 0; while (k < uboprobe_ntops && !(uboprobe_tops[k].address == e->address && uboprobe_tops[k].size == e->size && uboprobe_tops[k].cls == e->cls)) k++;
        if (k == uboprobe_ntops && k < 32) uboprobe_tops[uboprobe_ntops++] = (uboprobe_top){e->address, e->size, e->cls, 0};
        if (k < uboprobe_ntops) uboprobe_tops[k].changed++; }
    for (uint32_t i = 0; i < s->n; i++) free(s->e[i].copy);
    s->n = 0;
    if (presents >= uboprobe_last + 300) {
        if (uboprobe_last) { double f = (double)(presents - uboprobe_last);
            LOG("[uboprobe] frame=%llu per frame: %.0f uniform versions checked, %.1f changed before their batch finished (%.1f KB), %.1f unreadable",
                (unsigned long long)presents, uboprobe_checked / f, uboprobe_changed / f, uboprobe_changed_bytes / f / 1024, uboprobe_unreadable / f);
            for (int c = 0; c < UBOPROBE_CLASSES; c++) if (uboprobe_cls_checked[c])
                LOG("[uboprobe]   from %s: %.0f checked, %.1f changed", uboprobe_class_name[c], uboprobe_cls_checked[c] / f, uboprobe_cls_changed[c] / f);
            for (unsigned k = 0; k < uboprobe_ntops && k < 8; k++) {
                unsigned best = k; for (unsigned j = k + 1; j < uboprobe_ntops; j++) if (uboprobe_tops[j].changed > uboprobe_tops[best].changed) best = j;
                uboprobe_top t = uboprobe_tops[k]; uboprobe_tops[k] = uboprobe_tops[best]; uboprobe_tops[best] = t;
                LOG("[uboprobe]   %#llx+%u (%s) changed %.1f a frame", (unsigned long long)uboprobe_tops[k].address, uboprobe_tops[k].size, uboprobe_class_name[uboprobe_tops[k].cls], uboprobe_tops[k].changed / f); } }
        uboprobe_last = presents; uboprobe_checked = uboprobe_changed = uboprobe_changed_bytes = uboprobe_unreadable = 0; uboprobe_ntops = 0;
        memset(uboprobe_cls_checked, 0, sizeof uboprobe_cls_checked); memset(uboprobe_cls_changed, 0, sizeof uboprobe_cls_changed);
    }
}

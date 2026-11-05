// Cache read-only content versions and actual descriptor payloads within a batch; reset only after its GPU completes.
typedef struct uniform_version {
    uintptr_t address;
    uint32_t size;
    const uint8_t *mapped;
    VkDescriptorBufferInfo info;
    struct uniform_version *next;
    /* nonzero serial: these bytes came wholly from that submission's capture, read at post_write_order capture_order */
    uint64_t capture_serial, capture_order;
} uniform_version;
typedef struct uniform_version_chunk {
    uniform_version entries[64];
    unsigned used;
    struct uniform_version_chunk *next;
} uniform_version_chunk;
typedef struct descriptor_version {
    VkDescriptorSetLayout layout;
    VkDescriptorSet set;
    uint64_t hash,*payload;
    size_t words,capacity;
    struct descriptor_version *next;
} descriptor_version;
typedef struct descriptor_version_chunk {
    descriptor_version entries[64];
    unsigned used;
    struct descriptor_version_chunk *next;
} descriptor_version_chunk;
static uniform_version *uniform_versions[2048];
#include "nvn_vulkan_uboprobe.inc"
static uniform_version_chunk *uniform_version_chunks,*uniform_version_current;
static uint8_t uniform_read_scratch[65536];
typedef struct uniform_read_counters {
    uint64_t known_calls,known_bytes,safe_calls,safe_requested_bytes,safe_errors;
} uniform_read_counters;
static uniform_read_counters uniform_read_stats;
static descriptor_version *descriptor_versions[2048];
static descriptor_version_chunk *descriptor_version_chunks,*descriptor_version_current;
static uint64_t *descriptor_payload;
static size_t descriptor_payload_words,descriptor_payload_capacity;
static uint64_t descriptor_payload_hash;

/* served from an earlier read in the same submission */
static uint64_t uniform_version_reuse_hits;
static uint64_t uniform_version_hits,uniform_version_misses,uniform_version_read_bytes,uniform_version_compare_bytes,uniform_version_copy_bytes;
static uint64_t descriptor_version_hits,descriptor_version_misses,descriptor_memo_hits;
/* advances with every reset: per-shader descriptor memos (shader_entry.dmemo_*) hold sets of one generation */
static uint64_t binding_cache_generation;
static void binding_cache_reset(void) {
    binding_cache_generation++;
    memset(uniform_versions,0,sizeof uniform_versions);memset(descriptor_versions,0,sizeof descriptor_versions);
    for(uniform_version_chunk *c=uniform_version_chunks;c;c=c->next)c->used=0;uniform_version_current=uniform_version_chunks;
    for(descriptor_version_chunk *c=descriptor_version_chunks;c;c=c->next)c->used=0;descriptor_version_current=descriptor_version_chunks;
}
static uniform_version *uniform_version_new(void) {
    if(!uniform_version_current){uniform_version_chunks=calloc(1,sizeof *uniform_version_chunks);if(!uniform_version_chunks)die("Vulkan uniform version allocation");uniform_version_current=uniform_version_chunks;}
    while(uniform_version_current->used==64){if(!uniform_version_current->next){uniform_version_current->next=calloc(1,sizeof *uniform_version_current);if(!uniform_version_current->next)die("Vulkan uniform version allocation");}uniform_version_current=uniform_version_current->next;}
    return &uniform_version_current->entries[uniform_version_current->used++];
}
/* TOTK translation thread (TOTK_ASYNC_REPLAY=1): nvnQueueSubmitCommands returns before the replay, as on the console, and
   the game rebuilds its constant pools right away. The bound uniform ranges are captured in the submit call (game
   thread); the replay reads uniforms from that capture. A range the GPU is still writing at replay time is read from
   memory after the dependency wait instead. (Port of the Metal backend's mtl_sub_capture_ubos.) */
typedef struct { uint64_t lo, hi; } ucap_range;
/* The capture is made in GPU-visible memory (buf) and a uniform block it wholly holds is bound there (ucap_bind)
   instead of being read, compared and copied into the uniform arena at replay: 6% less translation CPU in the still
   scene. TOTK_VK_UCAPBIND=0 turns it off, =2 also checks each bound block against the arena path's bytes. Each range
   keeps its address's offset alignment, and 64 KiB of slack follows the last one (a dynamic uniform descriptor spans
   64 KiB). */
/* dirty[i]: a GPU write (a copy or a shader output) was pending over range i when the capture was installed, or one was
   registered since: the capture's bytes are not that block's, so it takes the arena path. Decided once per range at
   install and kept up to date by pp_add (ucap_mark_dirty), in place of two pending-page queries on every bind. A write
   that publishes later leaves the flag set: the capture was taken before that write landed, so the arena is right. */
typedef struct { int n; ucap_range *r; size_t *off; uint8_t *bytes; uint8_t *dirty; vk_buffer buf; uint64_t borrowed;
    /* copied: the bytes are in (mtl_sub_capture_copy, on the game thread, after the submission was queued) */
    _Atomic int copied, waiters; } ucap;
static ucap *ucap_current;
static int ucap_bind_state = -1;
static int ucap_bind_on_slow(void);
static inline int ucap_bind_on(void) { return ucap_bind_state >= 0 ? ucap_bind_state : ucap_bind_on_slow(); }
static int ucap_bind_on_slow(void) { int *onp = &ucap_bind_state; int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_UCAPBIND"); on = !e ? 1 : atoi(e) > 0 ? atoi(e) : 0; LOG("[vulkan] uniform blocks bound from the submit capture: %s", on == 2 ? "on, checked against the arena path" : on ? "on (TOTK_VK_UCAPBIND=0: off)" : "off"); } *onp = on; return on; }
static uint64_t ucap_bound, ucap_bound_bytes;
/* advances with every installed capture */
static uint64_t ucap_serial;
/* the last uniform_read_current was served wholly from the capture */
static int uniform_read_from_capture;
/* The same few hundred ranges are bound thousands of times per submission: an open-addressed set of the ranges seen
   so far keeps the list (and the sort after it, in the game's submit call on the frame's critical path) to the
   distinct ones. Slots carry the submission's generation, so the table is never cleared. */
typedef struct { uint64_t lo, hi; uint32_t gen, pad; } ucap_slot;
static __thread ucap_slot *ucap_set; static __thread uint32_t ucap_set_mask, ucap_gen;
/* 1 = already in the set */
static int ucap_insert(uint64_t lo, uint64_t hi) {
    for (uint32_t i = (uint32_t)((lo ^ hi * 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull >> 40) & ucap_set_mask;; i = (i + 1) & ucap_set_mask) {
        ucap_slot *s = &ucap_set[i];
        if (s->gen != ucap_gen) { *s = (ucap_slot){lo, hi, ucap_gen, 0}; return 0; }
        if (s->lo == lo && s->hi == hi) return 1;
    }
}
static int ucap_seen(const ucap_range *r, int n, uint64_t lo, uint64_t hi) {
    /* A/B: every bind kept */
    { static int off = -1; if (off < 0) off = exp_env("noucapdedup"); if (off) return 0; }
    /* grow, then put back this submission's ranges (r holds them, distinct) */
    if (!ucap_set || (uint32_t)n * 2 >= ucap_set_mask) {
        uint32_t size = ucap_set ? (ucap_set_mask + 1) * 2 : 8192;
        free(ucap_set); ucap_set = calloc(size, sizeof *ucap_set); if (!ucap_set) die("uniform capture set");
        ucap_set_mask = size - 1;
        for (int i = 0; i < n; i++) ucap_insert(r[i].lo, r[i].hi);
    }
    return ucap_insert(lo, hi);
}
int dm_android_read_ranges(int n, const uint64_t *ranges, uint8_t *base, const size_t *off);
/* hle_nvn.c: the ops EndRecording indexed */
extern const uint32_t *nvn_copy_op_index(uint64_t handle, uint32_t *n) __attribute__((weak));
/* hle_nvn.c: the recording's distinct uniform ranges, sorted (its ubo_range_t has this {lo, hi} layout), and its OP_CALL count */
extern const ucap_range *nvn_copy_ubo_ranges(uint64_t handle, uint32_t *n, uint32_t *calls) __attribute__((weak));
static void ucap_add(ucap_range **r, int *n, int *cap, const p_bind_buf *b) {
    if (b->addr && b->size && b->size <= 65536 && !ucap_seen(*r, *n, b->addr, b->addr + b->size)) {
        if (*n == *cap) { *cap = *cap ? *cap * 2 : 4096; *r = realloc(*r, (size_t)*cap * sizeof **r); if (!*r) die("uniform capture ranges"); }
        (*r)[(*n)++] = (ucap_range){b->addr, b->addr + b->size};
    }
}
static void ucap_collect(ucap_range **r, int *n, int *cap, const uint8_t *p, int depth) {
    uint32_t nx; const uint32_t *ix = nvn_copy_op_index ? nvn_copy_op_index((uint64_t)(uintptr_t)p, &nx) : NULL;
    { uint32_t nr, ncall; const ucap_range *list = ix && nvn_copy_ubo_ranges ? nvn_copy_ubo_ranges((uint64_t)(uintptr_t)p, &nr, &ncall) : NULL;
      /* the list EndRecording made (distinct, sorted); the merge below handles overlaps between recordings */
      if (list) {
          if (*n + (int)nr > *cap) { *cap = *n + (int)nr + 4096; *r = realloc(*r, (size_t)*cap * sizeof **r); if (!*r) die("uniform capture ranges"); }
          memcpy(*r + *n, list, (size_t)nr * sizeof **r); *n += (int)nr;
          if (ncall && depth < 16) for (uint32_t i = 0; i < nx && ncall; i++) {
              const uint8_t *q = p + ix[i]; op_hdr h; memcpy(&h, q, sizeof h);
              if (h.op == OP_CALL) { uint64_t t; memcpy(&t, q + 8, 8); ucap_collect(r, n, cap, (const uint8_t *)(uintptr_t)t, depth + 1); ncall--; } }
          return; } }
    /* only the indexed ops, not the whole recording */
    if (ix) {
        for (uint32_t i = 0; i < nx; i++) {
            const uint8_t *q = p + ix[i]; op_hdr h; memcpy(&h, q, sizeof h);
            if (h.op == OP_CALL) { uint64_t t; memcpy(&t, q + 8, 8); if (depth < 16) ucap_collect(r, n, cap, (const uint8_t *)(uintptr_t)t, depth + 1); }
            else if (h.op == OP_BIND_UBO) ucap_add(r, n, cap, (const p_bind_buf *)(q + 8));
        }
        return;
    }
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) die("corrupt recording at %p while capturing uniforms (op %u size %u)", (const void *)p, h.op, h.size);
        if (h.op == OP_END) return;
        if (h.op == OP_JUMP) { memcpy(&p, p + 8, 8); continue; }
        if (h.op == OP_CALL) { uint64_t t; memcpy(&t, p + 8, 8); if (depth < 16) ucap_collect(r, n, cap, (const uint8_t *)(uintptr_t)t, depth + 1); }
        else if (h.op == OP_BIND_UBO) ucap_add(r, n, cap, (const p_bind_buf *)(p + 8));
        p += h.size;
    }
}
/* Sort ranges by lo: an LSD radix sort over the 40 address bits (5 passes of 8). qsort with a comparator callback
   takes 0.25 ms of the submit call (on the game thread, ahead of translation) for a few thousand ranges. */
static void ucap_sort(ucap_range *r, int n) {
    static __thread ucap_range *tmp; static __thread int tmp_cap;
    if (n < 2) return;
    if (n > tmp_cap) { free(tmp); tmp_cap = n + n / 2; tmp = malloc((size_t)tmp_cap * sizeof *tmp); if (!tmp) die("uniform capture sort"); }
    uint64_t high = 0; for (int i = 0; i < n; i++) high |= r[i].lo;
    ucap_range *from = r, *to = tmp;
    for (unsigned shift = 0; shift < 64 && (high >> shift); shift += 8) {
        unsigned count[256] = {0};
        for (int i = 0; i < n; i++) count[(from[i].lo >> shift) & 255]++;
        unsigned sum = 0; for (unsigned k = 0; k < 256; k++) { unsigned c = count[k]; count[k] = sum; sum += c; }
        for (int i = 0; i < n; i++) to[count[(from[i].lo >> shift) & 255]++] = from[i];
        ucap_range *t = from; from = to; to = t;
    }
    if (from != r) memcpy(r, from, (size_t)n * sizeof *r);
}
/* where the submit call's time goes (on the game thread, ahead of the translation thread's start): logged every 600 captures */
static struct { uint64_t n, collect_ns, sort_ns, alloc_ns, copy_ns, ranges, bytes, waits, wait_ns; } ucap_prof;
void *mtl_sub_capture_ubos(const uint64_t *h, int n) {
    ucap_range *r = NULL; int count = 0, cap = 0;
    uint64_t t0 = dm_android_clock_ns(CLOCK_MONOTONIC), t1 = t0, t2 = t0, t3 = t0;
    /* 0 marks a never-used slot */
    if (!++ucap_gen) ucap_gen = 1;
    for (int i = 0; i < n; i++) ucap_collect(&r, &count, &cap, (const uint8_t *)(uintptr_t)h[i], 0);
    t1 = dm_android_clock_ns(CLOCK_MONOTONIC);
    ucap *c = calloc(1, sizeof *c); if (!c) die("uniform capture");
    if (count) {
        ucap_sort(r, count);
        int k = -1;
        /* the same ranges come every frame: ranges found inside a pool are remembered while the pools stand (pool_adds) */
        static struct { uint64_t lo, hi, pools; } known[4096]; uint64_t pools = atomic_load(&pool_adds);
        #define UCAP_IN_POOL(LO, HI) ({ uint64_t lo_ = (LO), hi_ = (HI); unsigned s_ = (unsigned)((lo_ ^ hi_ * 0x9E3779B97F4A7C15ull) >> 20) & 4095; int ok_; \
            if (known[s_].lo == lo_ && known[s_].hi == hi_ && known[s_].pools == pools) ok_ = 1; \
            else { ok_ = mtl_addr(lo_, hi_ - lo_, NULL, NULL); if (ok_) known[s_].lo = lo_, known[s_].hi = hi_, known[s_].pools = pools; } ok_; })
        for (int i = 0; i < count; i++) {
            if (k >= 0 && r[i].lo <= r[k].hi) { if (r[i].hi > r[k].hi && UCAP_IN_POOL(r[k].lo, r[i].hi)) r[k].hi = r[i].hi; if (r[i].hi <= r[k].hi) continue; }
            if (!UCAP_IN_POOL(r[i].lo, r[i].hi)) continue;
            r[++k] = r[i];
        }
        #undef UCAP_IN_POOL
        c->n = k + 1; size_t total = 0; int gpu = ucap_bind_on();
        uint64_t align = gpu && gpu_limits.minUniformBufferOffsetAlignment ? gpu_limits.minUniformBufferOffsetAlignment : 1;
        c->off = malloc(sizeof *c->off * (size_t)(c->n ? c->n : 1)); if (!c->off) die("uniform capture");
        /* offset = address (mod align) */
        for (int i = 0; i < c->n; i++) { total += (size_t)((r[i].lo - total) % align); c->off[i] = total; total += (size_t)(r[i].hi - r[i].lo); }
        t2 = dm_android_clock_ns(CLOCK_MONOTONIC);
        if (gpu) { c->buf = buffer_create(total + 65536); c->bytes = c->buf.mapped; }
        else { c->bytes = malloc(total ? total : 1); if (!c->bytes) die("uniform capture bytes"); }
        t3 = dm_android_clock_ns(CLOCK_MONOTONIC);
        /* the bytes follow in mtl_sub_capture_copy */
        c->r = r; ucap_prof.ranges += (uint64_t)c->n; ucap_prof.bytes += total;
    } else { free(r); atomic_store(&c->copied, 1); }
    ucap_prof.collect_ns += t1 - t0; ucap_prof.sort_ns += t2 - t1; ucap_prof.alloc_ns += t3 - t2;
    return c;
}
/* The bytes of the capture, read on the game thread after the submission was handed to the translation thread (the
   translation of the first draws, ~0.25 ms, overlaps this ~0.2 ms copy; the submit call still returns only once the
   bytes are in, so the game may rewrite its pools after the call). The translation thread waits for the bytes only
   where it reads them (ucap_wait: the CPU paths) and before a command buffer that may read them reaches the GPU
   (every vkQueueSubmit). */
#include <linux/futex.h>
#include <sys/syscall.h>
void mtl_sub_capture_copy(void *cap) {
    ucap *c = cap; if (!c || atomic_load(&c->copied)) return;
    uint64_t t0 = dm_android_clock_ns(CLOCK_MONOTONIC);
    /* {lo, hi} pairs, as ucap_range lays them out */
    dm_android_read_ranges(c->n, (const uint64_t *)c->r, c->bytes, c->off);
    atomic_store_explicit(&c->copied, 1, memory_order_release);
    if (atomic_load(&c->waiters)) syscall(SYS_futex, &c->copied, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
    ucap_prof.copy_ns += dm_android_clock_ns(CLOCK_MONOTONIC) - t0;
    if (++ucap_prof.n == 600) { LOG("[vulkan] submit capture per call: collect %.3f ms, sort+merge %.3f, buffer %.3f, copy %.3f (after the hand-off); %.0f ranges, %.2f MB; the translation waited for the bytes %.2f times, %.3f ms",
            ucap_prof.collect_ns / 600e6, ucap_prof.sort_ns / 600e6, ucap_prof.alloc_ns / 600e6, ucap_prof.copy_ns / 600e6, ucap_prof.ranges / 600.0, ucap_prof.bytes / 600.0 / 1048576,
            ucap_prof.waits / 600.0, ucap_prof.wait_ns / 600e6); memset(&ucap_prof, 0, sizeof ucap_prof); }
}
static void ucap_wait(const ucap *cc) {
    ucap *c = (ucap *)cc; if (!c || atomic_load_explicit(&c->copied, memory_order_acquire)) return;
    uint64_t t0 = dm_android_clock_ns(CLOCK_MONOTONIC);
    for (int i = 0; i < 2000 && !atomic_load_explicit(&c->copied, memory_order_acquire); i++) __asm__ volatile("yield");
    if (!atomic_load_explicit(&c->copied, memory_order_acquire)) {
        atomic_fetch_add(&c->waiters, 1);
        while (!atomic_load_explicit(&c->copied, memory_order_acquire)) { struct timespec ts = {0, 2000000}; syscall(SYS_futex, &c->copied, FUTEX_WAIT_PRIVATE, 0, &ts, NULL, 0); }
        atomic_fetch_sub(&c->waiters, 1);
    }
    ucap_prof.waits++; ucap_prof.wait_ns += dm_android_clock_ns(CLOCK_MONOTONIC) - t0;
}
static void ucap_wait_current(void) { ucap_wait(ucap_current); }
static void ucap_free(ucap *c) { if (c) { free(c->r); free(c->off); free(c->dirty); if (c->buf.buffer) buffer_destroy(&c->buf); else free(c->bytes); free(c); } }
/* the first range ending after address */
static int ucap_first(const ucap *c, uint64_t address) {
    int lo = 0, hi = c->n; while (lo < hi) { int m = (lo + hi) / 2; if (c->r[m].hi <= address) lo = m + 1; else hi = m; } return lo;
}
void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *cap) {
    (void)h; (void)n;
    /* kept until the next submission's replay (the queue lock serializes replays) */
    ucap_free(ucap_current); ucap_current = cap; ucap_serial++;
    /* the game's submission boundary: a sync point (the game waits on its fences) and the start of the piece
       ordinals (census) */
    game_sync_epoch++; sl.piece = 0; sl.submission++;
    ucap *c = cap;
    if (c && c->n) { c->dirty = calloc((size_t)c->n, 1); if (!c->dirty) die("uniform capture flags");
        for (int i = 0; i < c->n; i++) { uint64_t size = c->r[i].hi - c->r[i].lo; c->dirty[i] = async_unknown(c->r[i].lo, size) || async_copy_pending(c->r[i].lo, size); } }
}
/* a GPU write registered while this capture is current (pp_add): the ranges it touches leave the capture path */
static void ucap_mark_dirty(uintptr_t address, uint64_t bytes) {
    ucap *c = ucap_current; if (!c || !c->n || !c->dirty || !bytes || bytes > UINTPTR_MAX - address) return;
    for (int i = ucap_first(c, address); i < c->n && c->r[i].lo < address + bytes; i++) c->dirty[i] = 1;
}
/* Overlay the captured bytes of [address, address+size) onto out. */
static void ucap_apply(uintptr_t address, uint8_t *out, uint32_t size) {
    const ucap *c = ucap_current; if (!c || !c->n) return; ucap_wait(c);
    int lo = 0, hi = c->n; while (lo < hi) { int m = (lo + hi) / 2; if (c->r[m].hi <= address) lo = m + 1; else hi = m; }
    for (int i = lo; i < c->n && c->r[i].lo < address + size; i++) {
        uint64_t a = c->r[i].lo > address ? c->r[i].lo : address, b = c->r[i].hi < address + size ? c->r[i].hi : address + size;
        if (a < b) memcpy(out + (a - address), c->bytes + c->off[i] + (a - c->r[i].lo), (size_t)(b - a));
    }
}
/* ucap_bind: [address, address+size) wholly in the GPU-visible capture, with no known copy or GPU write pending over it
   (those bytes are not the capture's: uniform_read_current overlays them). The capture's buffer is borrowed once per
   batch, so it outlives every batch that binds it. */
static int uniform_read_current(uintptr_t address,uint32_t size);
/* why a uniform block took the arena path instead (counted per frame in ucap_bind's report) */
enum { UCAP_NO_CAPTURE, UCAP_EXTENT_UNKNOWN, UCAP_READS_PAST, UCAP_TAA, UCAP_NOT_COVERED, UCAP_MISALIGNED, UCAP_GPU_PENDING, UCAP_COPY_PENDING, UCAP_REASONS };
static const char *const ucap_reason_name[UCAP_REASONS] = {"no GPU capture", "extent unknown, bound below 64 KiB", "reads past the bound size", "TAA", "not in the capture", "misaligned", "GPU write pending", "copy pending"};
static uint64_t ucap_missed[UCAP_REASONS], ucap_missed_bytes[UCAP_REASONS];
static void ucap_miss(int why, uint32_t size) { ucap_missed[why]++; ucap_missed_bytes[why] += size; }
/* the same blocks are bound draw after draw: remember each address's range for this capture */
static struct { uint64_t address, serial; int index; } ucap_memo[1024];
static int ucap_range_of(const ucap *c, uint64_t address, ucap_memo_entry *m) {
    /* this binding's block, as in the last draw */
    if (m) { if (m->serial == ucap_serial && m->address == address) return m->index;
        /* a new capture: the same block is usually at the same index (the range lists differ little frame to frame) */
        int lo = m->address == address && m->index < c->n && c->r[m->index].lo <= address && address < c->r[m->index].hi ? m->index : ucap_first(c, address);
        *m = (ucap_memo_entry){address, ucap_serial, lo}; return lo; }
    unsigned slot = (unsigned)((address >> 4) * 0x9E3779B97F4A7C15ull >> 54);
    if (ucap_memo[slot].serial == ucap_serial && ucap_memo[slot].address == address) return ucap_memo[slot].index;
    int lo = ucap_first(c, address); ucap_memo[slot].address = address; ucap_memo[slot].serial = ucap_serial; ucap_memo[slot].index = lo; return lo;
}
/* range: the descriptor's (65536, or the exact bound bytes under robustBufferAccess2) */
static int ucap_bind(uintptr_t address, uint32_t size, uint32_t range, VkDescriptorBufferInfo *info, ucap_memo_entry *m) {
    ucap *c = ucap_current; if (!c || !c->n || !c->buf.buffer) { ucap_miss(UCAP_NO_CAPTURE, size); return 0; }
    int lo = ucap_range_of(c, address, m);
    if (lo >= c->n || c->r[lo].lo > address || c->r[lo].hi < (uint64_t)address + size) { ucap_miss(UCAP_NOT_COVERED, size); return 0; }
    uint64_t offset = c->off[lo] + (address - c->r[lo].lo);
    if (gpu_limits.minUniformBufferOffsetAlignment && offset % gpu_limits.minUniformBufferOffsetAlignment) { ucap_miss(UCAP_MISALIGNED, size); return 0; }
    if (c->dirty[lo]) { ucap_miss(UCAP_GPU_PENDING, size); return 0; }
    if (c->borrowed != post_batch_serial) { post_new(3)->source = buffer_borrow(c->buf); c->borrowed = post_batch_serial; }
    ucap_bound++; ucap_bound_bytes += size;
    /* TOTK_VK_UCAPBIND=2: what the arena path would have read */
    if (ucap_bind_on() == 2) { static uint64_t checked, differ; checked++;
        if (ucap_wait(c), !uniform_read_current(address, size) && memcmp(uniform_read_scratch, c->bytes + offset, size)) { differ++;
            if (differ <= 50) LOG("[vulkan] uniform capture bind differs from the arena path: %#llx+%u (%llu of %llu checked)", (unsigned long long)address, size, (unsigned long long)differ, (unsigned long long)checked); } }
    { static uint64_t last, b0, n0; if (presents >= last + 600) { if (last) { LOG("[vulkan] uniform blocks bound from the submit capture: %.0f a frame, %.2f MB a frame", (ucap_bound - n0) / 600.0, (ucap_bound_bytes - b0) / 600.0 / 1048576);
            for (int k = 0; k < UCAP_REASONS; k++) if (ucap_missed[k]) LOG("[vulkan]   arena path, %s: %.0f a frame, %.2f MB a frame", ucap_reason_name[k], ucap_missed[k] / 600.0, ucap_missed_bytes[k] / 600.0 / 1048576); }
            memset(ucap_missed, 0, sizeof ucap_missed); memset(ucap_missed_bytes, 0, sizeof ucap_missed_bytes); last = presents; n0 = ucap_bound; b0 = ucap_bound_bytes; } }
    *info = (VkDescriptorBufferInfo){c->buf.buffer, offset, range}; return 1;
}
/* TOTK_VK_INPLACE=2: a uniform block bound in place is compared with the submit capture's bytes (the block as it was at
   the submit call); a difference means the game wrote it after the submit, before the GPU could read it in place: a
   block for the keep list. Counts, and the first differing blocks. */
static uint64_t inplace_checked, inplace_differ;
static void inplace_check(uintptr_t address, uint32_t size, ucap_memo_entry *m) {
    ucap *c = ucap_current; if (!c || !c->n || !c->bytes) return; ucap_wait(c);
    int lo = ucap_range_of(c, address, m);
    if (lo >= c->n || c->r[lo].lo > address || c->r[lo].hi < (uint64_t)address + size) return;
    uint64_t off = c->off[lo] + (address - c->r[lo].lo); inplace_checked++;
    /* for the check at completion: the entry, and the capture kept alive by this batch */
    if (async_current && c->buf.buffer) {
        async_batch *b = async_current; if (b->ipp_n == b->ipp_cap) { b->ipp_cap = b->ipp_cap ? b->ipp_cap * 2 : 4096; b->ipp = realloc(b->ipp, b->ipp_cap * sizeof *b->ipp); if (!b->ipp) die("in-place probe"); }
        b->ipp[b->ipp_n++] = (struct ipp_entry){address, size, c->bytes + off};
        if (c->borrowed != post_batch_serial) { post_new(3)->source = buffer_borrow(c->buf); c->borrowed = post_batch_serial; } }
    if (memcmp(c->bytes + off, (const void *)address, size)) { inplace_differ++;
        static unsigned said; if (said++ < 60) { size_t k = 0; while (k < size && c->bytes[off + k] == ((const uint8_t *)address)[k]) k++;
            LOG("[vulkan-inplace-check] frame %llu block %#llx+%u differs from the submit capture from byte %zu (%llu of %llu checked differ)", (unsigned long long)presents, (unsigned long long)address, size, k, (unsigned long long)inplace_differ, (unsigned long long)inplace_checked); } }
}
/* [address, address+size) is wholly in the current capture and no GPU write was pending over its range: the capture
   path will serve it, so no pending-write query is needed for it. */
static int ucap_clean(uintptr_t address, uint32_t size, ucap_memo_entry *m) {
    const ucap *c = ucap_current; if (!c || !c->n || !c->dirty) return 0;
    int lo = ucap_range_of(c, address, m);
    return lo < c->n && c->r[lo].lo <= address && c->r[lo].hi >= (uint64_t)address + size && !c->dirty[lo];
}
/* The capture holds all of [address, address+size) in one merged range. */
static int ucap_covers(uintptr_t address, uint32_t size) {
    const ucap *c = ucap_current; if (!c || !c->n) return 0;
    int lo = 0, hi = c->n; while (lo < hi) { int m = (lo + hi) / 2; if (c->r[m].hi <= address) lo = m + 1; else hi = m; }
    return lo < c->n && c->r[lo].lo <= address && c->r[lo].hi >= (uint64_t)address + size;
}
/* uboprobe: where the last read's bytes came from (UBOPROBE_*) */
static int uniform_read_class; static uint64_t uniform_read_calls;
static int uniform_read_current(uintptr_t address,uint32_t size) {
    uniform_read_from_capture=0;uniform_read_calls++;uniform_read_class=UBOPROBE_KNOWN_COPY;
    // The destination is only queue-owned scratch, never passed to the GPU or aliased with guest/arena memory; the caller checks the full range.
    if(async_read_known_full_copy(address,uniform_read_scratch,size)){
        uniform_read_stats.known_calls++;uniform_read_stats.known_bytes+=size;
    }else{
        int unknown=ucap_current?async_unknown(address,size):1;
        uniform_read_from_capture=!unknown&&ucap_covers(address,size);
        uniform_read_class=unknown?UBOPROBE_GPU_PENDING:uniform_read_from_capture?UBOPROBE_CAPTURE:UBOPROBE_MEMORY;
        /* wholly captured at the submit call: no guest read */
        if(uniform_read_from_capture)ucap_apply(address,uniform_read_scratch,size);
        else{
        uniform_read_stats.safe_calls++;uniform_read_stats.safe_requested_bytes+=size;
        if(dm_android_read_memory(address,uniform_read_scratch,size)){uniform_read_stats.safe_errors++;return -1;}
        /* as of the submit call */
        if(!unknown)ucap_apply(address,uniform_read_scratch,size);
        }
        // Overlay every known pending copy in the original FIFO order.
        async_overlay_copies(address,uniform_read_scratch,size);
    }
    snapshot_read_account(size);return 0;
}
static VkDescriptorBufferInfo uniform_read_cached_read(uintptr_t address,uint32_t size);
/* nvn_vulkan_taa.inc: the camera's jitter (our copy only) */
static void taa_uniform(uintptr_t address,uint32_t size,uint8_t *data);
static VkDescriptorBufferInfo uniform_read_cached(uintptr_t address,uint32_t size) {
    uint64_t calls=uniform_read_calls,phase=dm_cpu_phase_begin();VkDescriptorBufferInfo info=uniform_read_cached_read(address,size);dm_cpu_phase_end(DM_CPU_PHASE_UBO,phase);
    if(uboprobe_on()){uniform_version *e=uniform_versions[read_bucket(address,size)&2047];while(e&&(e->address!=address||e->size!=size))e=e->next;
        /* no read: reused from this submission's capture */
        int c=calls==uniform_read_calls?UBOPROBE_CAPTURE:uniform_read_class;
        if(c!=UBOPROBE_KNOWN_COPY&&c!=UBOPROBE_GPU_PENDING&&async_copy_pending(address,size))c=UBOPROBE_COPY_OVERLAY;
        if(e)uboprobe_note(address,size,e->mapped,c);}
    return info;
}
static VkDescriptorBufferInfo uniform_read_cached_read(uintptr_t address,uint32_t size) {
    if(!address||!size||size>65536||size>UINTPTR_MAX-address)die("Vulkan uniform version bounds");
    // queue_lock serializes scratch reuse; compare fresh safe-read/FIFO-overlay bytes without relying on page protection or old stamps.
    unsigned bucket=read_bucket(address,size)&2047;uniform_version *entry=uniform_versions[bucket];while(entry&&(entry->address!=address||entry->size!=size))entry=entry->next;
    /* read earlier in this submission from the capture, and nothing recorded since could overlay it: same bytes */
    if(entry&&ucap_current&&entry->capture_serial==ucap_serial&&entry->capture_order==post_write_order){uniform_version_reuse_hits++;
        /* the reuse rule against a fresh read */
        static int check=-1;if(check<0)check=exp_env("ubocheck");
        if(check&&!uniform_read_current(address,size)&&memcmp(entry->mapped,uniform_read_scratch,size))die("Vulkan uniform reuse mismatch: %#llx+%u",(unsigned long long)address,size);
        return entry->info;}
    if(uniform_read_current(address,size))die("Unreadable Vulkan cached uniform");uniform_version_read_bytes+=size;
    taa_uniform(address,size,uniform_read_scratch);
    uint64_t serial=uniform_read_from_capture?ucap_serial:0;
    if(entry){uniform_version_compare_bytes+=size;if(!memcmp(entry->mapped,uniform_read_scratch,size)){uniform_version_hits++;entry->capture_serial=serial;entry->capture_order=post_write_order;return entry->info;}}
    uniform_version_misses++;uint8_t *mapped;VkDescriptorBufferInfo info=uniform_allocate_overwrite(size,&mapped);
    // Allocate a fresh arena slot on changes; bytes referenced by older descriptors stay immutable until this batch's fence completes.
    memcpy(mapped,uniform_read_scratch,size);uniform_version_copy_bytes+=size;
    if(!entry){entry=uniform_version_new();entry->address=address;entry->size=size;entry->next=uniform_versions[bucket];uniform_versions[bucket]=entry;}
    entry->mapped=mapped;entry->info=info;entry->capture_serial=serial;entry->capture_order=post_write_order;
    return info;
}
static void descriptor_payload_build(const VkWriteDescriptorSet *writes,unsigned count) {
    size_t words=0;for(unsigned i=0;i<count;i++){VkDescriptorType type=writes[i].descriptorType;words+=2+writes[i].descriptorCount*((type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER||type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)?1:3);}
    if(words>descriptor_payload_capacity){descriptor_payload=realloc(descriptor_payload,words*sizeof *descriptor_payload);if(!descriptor_payload)die("Vulkan descriptor payload allocation");descriptor_payload_capacity=words;}
    size_t cursor=0;for(unsigned i=0;i<count;i++){const VkWriteDescriptorSet *w=&writes[i];descriptor_payload[cursor++]=((uint64_t)w->descriptorType<<32)|w->dstBinding;descriptor_payload[cursor++]=w->descriptorCount;
        for(unsigned j=0;j<w->descriptorCount;j++){
            if(descriptor_type_uniform(w->descriptorType)||w->descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER){const VkDescriptorBufferInfo *b=&w->pBufferInfo[j];descriptor_payload[cursor++]=(uintptr_t)b->buffer;descriptor_payload[cursor++]=b->offset;descriptor_payload[cursor++]=b->range;}
            else if(w->descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER||w->descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)descriptor_payload[cursor++]=(uintptr_t)w->pTexelBufferView[j];
            else {const VkDescriptorImageInfo *image=&w->pImageInfo[j];descriptor_payload[cursor++]=(uintptr_t)image->sampler;descriptor_payload[cursor++]=(uintptr_t)image->imageView;descriptor_payload[cursor++]=image->imageLayout;}
        }
    }
    descriptor_payload_words=cursor;
    // Encode explicit fields rather than treating unspecified Vulkan structure padding as content. Four independent
    // lanes, folded at the end, so the multiplies do not form one serial dependency chain (a serial FNV loop is ~12% of
    // bind_shader's self time).
    const uint64_t *w=descriptor_payload;size_t n=descriptor_payload_words,i=0;
    uint64_t h0=14695981039346656037ull,h1=h0^1,h2=h0^2,h3=h0^3;
    for(;i+4<=n;i+=4){h0=(h0^w[i])*1099511628211ull;h1=(h1^w[i+1])*1099511628211ull;h2=(h2^w[i+2])*1099511628211ull;h3=(h3^w[i+3])*1099511628211ull;}
    for(;i<n;i++)h0=(h0^w[i])*1099511628211ull;
    uint64_t h=h0^(h1*0x9E3779B97F4A7C15ull)^(h2*0xC2B2AE3D27D4EB4Full)^(h3*0x165667B19E3779F9ull)^n;
    // Arena offsets share low 64 KiB bits; mix high bits at the end so model versions do not all land in one bucket.
    h^=h>>33;h*=0xff51afd7ed558ccdull;h^=h>>33;
    descriptor_payload_hash=h;
}
static VkDescriptorSet descriptor_cache_find_read(VkDescriptorSetLayout layout,const VkWriteDescriptorSet *writes,unsigned count);
static VkDescriptorSet descriptor_cache_find(VkDescriptorSetLayout layout,const VkWriteDescriptorSet *writes,unsigned count) {
    uint64_t phase=dm_cpu_phase_begin();VkDescriptorSet set=descriptor_cache_find_read(layout,writes,count);dm_cpu_phase_end(DM_CPU_PHASE_DESCRIPTOR,phase);return set;
}
static VkDescriptorSet descriptor_cache_find_read(VkDescriptorSetLayout layout,const VkWriteDescriptorSet *writes,unsigned count) {
    descriptor_payload_build(writes,count);unsigned bucket=(descriptor_payload_hash^(uintptr_t)layout)&2047;
    for(descriptor_version *entry=descriptor_versions[bucket];entry;entry=entry->next)
        if(entry->layout==layout&&entry->hash==descriptor_payload_hash&&entry->words==descriptor_payload_words&&(!entry->words||!memcmp(entry->payload,descriptor_payload,entry->words*sizeof *entry->payload))){descriptor_version_hits++;return entry->set;}
    descriptor_version_misses++;return VK_NULL_HANDLE;
}
static void descriptor_cache_store(VkDescriptorSetLayout layout,VkDescriptorSet set) {
    uint64_t phase=dm_cpu_phase_begin();
    if(!descriptor_version_current){descriptor_version_chunks=calloc(1,sizeof *descriptor_version_chunks);if(!descriptor_version_chunks)die("Vulkan descriptor version allocation");descriptor_version_current=descriptor_version_chunks;}
    while(descriptor_version_current->used==64){if(!descriptor_version_current->next){descriptor_version_current->next=calloc(1,sizeof *descriptor_version_current);if(!descriptor_version_current->next)die("Vulkan descriptor version allocation");}descriptor_version_current=descriptor_version_current->next;}
    descriptor_version *entry=&descriptor_version_current->entries[descriptor_version_current->used++];
    if(descriptor_payload_words>entry->capacity){entry->payload=realloc(entry->payload,descriptor_payload_words*sizeof *entry->payload);if(!entry->payload)die("Vulkan descriptor version payload allocation");entry->capacity=descriptor_payload_words;}
    entry->layout=layout;entry->set=set;entry->hash=descriptor_payload_hash;entry->words=descriptor_payload_words;
    if(entry->words)memcpy(entry->payload,descriptor_payload,entry->words*sizeof *entry->payload);
    unsigned bucket=(entry->hash^(uintptr_t)layout)&2047;entry->next=descriptor_versions[bucket];descriptor_versions[bucket]=entry;
    dm_cpu_phase_end(DM_CPU_PHASE_DESCRIPTOR,phase);
}

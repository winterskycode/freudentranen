/* Deferred command recording (TOTK_VK_DEFER, default on): the vkCmd* calls, command buffer begin / end / reset and queue
   submits of this file are written as compact records into chunks, and a worker thread ("totk-vkrec") replays them into
   the driver in order. The translation thread keeps the state work and the guest-memory snapshots; Turnip's recording
   (~22% of the replay) runs beside it instead of on the frame's critical path (Eden's Scheduler, DXVK's CS thread).

   Ordering rules:
   - every record goes through one FIFO (any thread may record; recording threads hold queue_lock, so one at a time);
   - what records refer to must outlive their replay: objects are destroyed after a fence, i.e. after their submit
     was replayed; arrays and structs passed by pointer are copied into the record;
   - anything that touches the queue or a command buffer outside the records first waits for the worker to finish
     (dv_drain): presents, queue / device idle waits, the overlay (records straight into the current command buffer)
     and LSFG (submits on its own);
   - vkResetFences stays immediate: a batch's fence is reset only after its previous submit completed. */
#include <sched.h>
enum {
    DV_PIPELINE_BARRIER = 1, DV_COPY_BUFFER, DV_COPY_IMAGE_TO_BUFFER, DV_COPY_IMAGE, DV_BLIT_IMAGE, DV_BIND_PIPELINE,
    DV_BIND_DESCRIPTOR_SETS, DV_WRITE_TIMESTAMP, DV_RESET_QUERY_POOL, DV_DISPATCH, DV_CLEAR_COLOR_IMAGE, DV_PUSH_CONSTANTS,
    DV_FILL_BUFFER, DV_COPY_BUFFER_TO_IMAGE, DV_END_RENDER_PASS, DV_END_QUERY, DV_COPY_QUERY_POOL_RESULTS,
    DV_BEGIN_RENDER_PASS, DV_BEGIN_QUERY, DV_CLEAR_DEPTH_STENCIL_IMAGE, DV_CLEAR_ATTACHMENTS, DV_SET_VIEWPORT,
    DV_SET_STENCIL_WRITE_MASK, DV_SET_STENCIL_REFERENCE, DV_SET_STENCIL_COMPARE_MASK, DV_SET_SCISSOR, DV_SET_DEPTH_BIAS,
    DV_SET_BLEND_CONSTANTS, DV_DRAW_INDEXED, DV_DRAW, DV_BIND_VERTEX_BUFFERS, DV_BIND_INDEX_BUFFER,
    DV_EDS_DEPTH_TEST, DV_EDS_DEPTH_WRITE, DV_EDS_DEPTH_COMPARE, DV_EDS_STENCIL_TEST, DV_EDS_STENCIL_OP, DV_EDS_CULL,
    DV_EDS_FRONT, DV_EDS_BIAS_ENABLE,
    DV_BEGIN_COMMAND_BUFFER, DV_END_COMMAND_BUFFER, DV_RESET_COMMAND_BUFFER, DV_QUEUE_SUBMIT,
};
/* size: the whole record, 8-byte aligned */
typedef struct { uint32_t op, size; VkCommandBuffer cb; } dv_hdr;
#define DV_CHUNK_BYTES (64u << 10)
/* Spin before sleeping, 200 us: each publish to a sleeping worker is a futex wake on
   the translation thread (~68 a frame), and this worker has prime core 6 to itself (fast:1). Still scene, two rounds:
   0 us 18.5/18.3 ms, 50 us 18.3/18.5, 200 us 17.4/17.9 (p99 ~26 -> ~23.8 ms, translation CPU 10.6 -> 10.1 ms). Sharing
   both prime cores with translation, spinning would take a core from the game's main thread (wake -> submit
   0.9 -> 1.4 ms): it needs the core split. */
#define DV_SPIN_NS 200000ull
/* The spin buys about 1 ms of frame time for most of this thread's CPU, which only matters running flat out. With a cap of
   60 or less it is off (Odin: 40 fps 11.4 vs 12.1 W, 60 fps 19.6 vs 20.2 W, the same frame rate and p99).
   TOTK_VKREC_SPIN_US forces a value for measurements. */
extern int nvn_frame_cap_get(void) __attribute__((weak));
static uint64_t dv_spin_ns(void) {
    static int64_t forced = -2;
    if (forced == -2) { const char *e = getenv("TOTK_VKREC_SPIN_US"); forced = e ? atoll(e) * 1000 : -1; if (e) LOG("[vulkan-defer] worker spin forced to %lld us", (long long)(forced / 1000)); }
    if (forced >= 0) return (uint64_t)forced;
    return nvn_frame_cap_get && nvn_frame_cap_get() <= 60 ? 0 : DV_SPIN_NS;
}
/* hand a chunk over every ~40 draws: often enough for the GPU, few mutex round trips */
#define DV_PUBLISH_BYTES (8u << 10)
typedef struct dv_chunk { struct dv_chunk *next; uint32_t used; _Alignas(16) uint8_t data[DV_CHUNK_BYTES]; } dv_chunk;
static struct {
    /* -1 unread */
    int on;
    pthread_mutex_t mu; pthread_cond_t work, idle;
    dv_chunk *head, *tail, *free_list, *cur;
    /* chunks */
    uint64_t published, replayed;
    int worker_waiting;
    uint64_t records, bytes, drains, drain_ns;
} dv = {.on = -1, .mu = PTHREAD_MUTEX_INITIALIZER, .work = PTHREAD_COND_INITIALIZER, .idle = PTHREAD_COND_INITIALIZER};

static void dv_replay(const uint8_t *p, const uint8_t *end);
/* the recording worker's core: the prime core the translation thread does not use (TOTK_VKREC_CPUS, "fast:1" by default;
   see totk_affinity.h and x_thread_pin) */
static void dv_pin_fast_cores(void) {
#ifdef __ANDROID__
    totk_pin_self("TOTK_VKREC_CPUS", "fast:1");
    dm_android_perf_hint_thread();
#endif
}
static void *dv_worker(void *unused) {
    (void)unused; pthread_setname_np(pthread_self(), "totk-vkrec"); dv_pin_fast_cores();
    for (;;) {
        /* a frame's chunks arrive every few microseconds: spin a little before sleeping (each sleep and wake-up cost
           more than the chunk) */
        if (!__atomic_load_n(&dv.head, __ATOMIC_ACQUIRE)) { uint64_t t0 = dm_android_clock_ns(CLOCK_MONOTONIC), limit = dv_spin_ns();
            while (!__atomic_load_n(&dv.head, __ATOMIC_ACQUIRE) && dm_android_clock_ns(CLOCK_MONOTONIC) - t0 < limit) __asm__ volatile("yield"); }
        pthread_mutex_lock(&dv.mu);
        while (!dv.head) { dv.worker_waiting = 1; pthread_cond_wait(&dv.work, &dv.mu); dv.worker_waiting = 0; }
        dv_chunk *c = dv.head; dv.head = c->next; if (!dv.head) dv.tail = NULL;
        pthread_mutex_unlock(&dv.mu);
        dv_replay(c->data, c->data + c->used);
        pthread_mutex_lock(&dv.mu);
        c->used = 0; c->next = dv.free_list; dv.free_list = c; dv.replayed++;
        if (dv.replayed == dv.published) pthread_cond_broadcast(&dv.idle);
        pthread_mutex_unlock(&dv.mu);
    }
    return NULL;
}
static int dv_enabled_slow(void);
static inline int dv_enabled(void) { return dv.on >= 0 ? dv.on : dv_enabled_slow(); }
static int dv_enabled_slow(void) {
    if (dv.on >= 0) return dv.on;
    const char *e = getenv("TOTK_VK_DEFER"); dv.on = !(e && !atoi(e));
    if (dv.on) { pthread_t t; if (pthread_create(&t, NULL, dv_worker, NULL)) die("Vulkan deferred recording thread"); pthread_detach(t); }
    LOG("[vulkan-defer] command recording on %s", dv.on ? "a worker thread (TOTK_VK_DEFER=0: inline)" : "the calling thread");
    return dv.on;
}
/* holding dv.mu */
static void dv_publish_locked(void) {
    dv_chunk *c = dv.cur; if (!c || !c->used) return;
    dv.cur = NULL; c->next = NULL; if (dv.tail) dv.tail->next = c; else __atomic_store_n(&dv.head, c, __ATOMIC_RELEASE); dv.tail = c; dv.published++;
    if (dv.worker_waiting) pthread_cond_signal(&dv.work);
}
static void dv_publish(void) { pthread_mutex_lock(&dv.mu); dv_publish_locked(); pthread_mutex_unlock(&dv.mu); }
/* Everything recorded so far has reached the driver. */
static void dv_drain(void) {
    if (dv.on <= 0) return;
    pthread_mutex_lock(&dv.mu); dv_publish_locked();
    if (dv.replayed != dv.published) { uint64_t t0 = dm_android_clock_ns(CLOCK_MONOTONIC);
        while (dv.replayed != dv.published) pthread_cond_wait(&dv.idle, &dv.mu);
        dv.drain_ns += dm_android_clock_ns(CLOCK_MONOTONIC) - t0; }
    pthread_mutex_unlock(&dv.mu);
    /* the presents drain once a frame: every 300 of them, what the recorder did per frame */
    if (!(++dv.drains % 300)) { static uint64_t r0, b0, ns0, ch0;
        LOG("[vulkan-defer] per frame (300 drains): %.0f records, %.1f KiB, %.1f chunks, drain wait %.3f ms",
            (dv.records - r0) / 300.0, (dv.bytes - b0) / 300.0 / 1024, (dv.published - ch0) / 300.0, (dv.drain_ns - ns0) / 300e6);
        r0 = dv.records; b0 = dv.bytes; ns0 = dv.drain_ns; ch0 = dv.published; }
}
/* GPU commands outside render passes a frame (the GPU time between passes: batch spans minus pass sums), by kind;
   logged every 600 presents next to the pass timer's totals */
static uint64_t dvc_count[64][2]; static int dvc_inpass;
static void dv_census(uint32_t op) {
    if (op == DV_BEGIN_RENDER_PASS) dvc_inpass = 1; else if (op == DV_END_RENDER_PASS) dvc_inpass = 0;
    if (op < 64) dvc_count[op][dvc_inpass]++;
}
static void dv_census_report(uint64_t frames) {
    static const struct { uint32_t op; const char *name; } kinds[] = {{DV_PIPELINE_BARRIER, "barriers"}, {DV_COPY_BUFFER, "buffer copies"}, {DV_COPY_IMAGE, "image copies"}, {DV_COPY_BUFFER_TO_IMAGE, "uploads"},
        {DV_COPY_IMAGE_TO_BUFFER, "readbacks"}, {DV_BLIT_IMAGE, "blits"}, {DV_CLEAR_COLOR_IMAGE, "colour clears"}, {DV_CLEAR_DEPTH_STENCIL_IMAGE, "depth clears"}, {DV_FILL_BUFFER, "fills"}, {DV_DISPATCH, "dispatches"},
        {DV_COPY_QUERY_POOL_RESULTS, "query copies"}, {DV_WRITE_TIMESTAMP, "timestamps"}, {DV_BEGIN_RENDER_PASS, "passes"}, {DV_QUEUE_SUBMIT, "submits"}};
    char line[640]; int k = 0;
    for (unsigned i = 0; i < sizeof kinds / sizeof *kinds && k < (int)sizeof line - 48; i++) { uint64_t out = dvc_count[kinds[i].op][0], in = dvc_count[kinds[i].op][1];
        if (out || in) k += snprintf(line + k, sizeof line - k, " %s %.0f%s", kinds[i].name, (double)out / frames, in ? "" : ""); if (in && kinds[i].op == DV_PIPELINE_BARRIER) k += snprintf(line + k, sizeof line - k, " (+%.0f in passes)", (double)in / frames); }
    LOG("[gpu-outside] a frame, recorded outside render passes:%s", line);
    memset(dvc_count, 0, sizeof dvc_count);
}
static void *dv_alloc(uint32_t op, VkCommandBuffer cb, size_t bytes) {
    bytes = (sizeof(dv_hdr) + bytes + 7) & ~(size_t)7;
    if (bytes > DV_CHUNK_BYTES) die("Vulkan deferred record of %zu bytes", bytes);
    /* the records so far are complete here: hand them over every DV_PUBLISH_BYTES so the worker keeps pace */
    if (dv.cur && (dv.cur->used >= DV_PUBLISH_BYTES || dv.cur->used + bytes > DV_CHUNK_BYTES)) dv_publish();
    if (!dv.cur) {
        pthread_mutex_lock(&dv.mu); dv_chunk *c = dv.free_list; if (c) dv.free_list = c->next; pthread_mutex_unlock(&dv.mu);
        if (!c) { c = aligned_alloc(16, sizeof *c); if (!c) die("Vulkan deferred chunk"); }
        c->used = 0; c->next = NULL; dv.cur = c;
    }
    dv_hdr *h = (dv_hdr *)(dv.cur->data + dv.cur->used); h->op = op; h->size = (uint32_t)bytes; h->cb = cb;
    dv.cur->used += (uint32_t)bytes; dv.records++; dv.bytes += bytes;
    dv_census(op);
    return h + 1;
}
/* after a record that should reach the GPU soon (submits) or once enough has gathered */
static void dv_maybe_publish(int now) { if (dv.cur && (now || dv.cur->used >= DV_PUBLISH_BYTES)) dv_publish(); }
#define DV_COPY(dst, src, n) do { if (n) memcpy((dst), (src), (n)); } while (0)

/* ---- wrappers: record when deferring, else call straight through ---- */
#define DV_REAL(fn) (vk_api.fn)
static void dv_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
    uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    if (!dv_enabled()) { DV_REAL(vkCmdPipelineBarrier)(cb, src, dst, dep, nm, m, nb, b, ni, im); return; }
    struct { VkPipelineStageFlags src, dst; VkDependencyFlags dep; uint32_t nm, nb, ni; } *r;
    size_t sm = nm * sizeof *m, sb = nb * sizeof *b, si = ni * sizeof *im;
    r = dv_alloc(DV_PIPELINE_BARRIER, cb, sizeof *r + sm + sb + si); r->src = src; r->dst = dst; r->dep = dep; r->nm = nm; r->nb = nb; r->ni = ni;
    uint8_t *t = (uint8_t *)(r + 1); DV_COPY(t, m, sm); DV_COPY(t + sm, b, sb); DV_COPY(t + sm + sb, im, si);
}
static void dv_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *rg) {
    if (!dv_enabled()) { DV_REAL(vkCmdCopyBuffer)(cb, s, d, n, rg); return; }
    struct { VkBuffer s, d; uint32_t n; } *r = dv_alloc(DV_COPY_BUFFER, cb, sizeof *r + n * sizeof *rg); r->s = s; r->d = d; r->n = n; DV_COPY(r + 1, rg, n * sizeof *rg);
}
static void dv_vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage s, VkImageLayout l, VkBuffer d, uint32_t n, const VkBufferImageCopy *rg) {
    if (!dv_enabled()) { DV_REAL(vkCmdCopyImageToBuffer)(cb, s, l, d, n, rg); return; }
    struct { VkImage s; VkImageLayout l; VkBuffer d; uint32_t n; } *r = dv_alloc(DV_COPY_IMAGE_TO_BUFFER, cb, sizeof *r + n * sizeof *rg);
    r->s = s; r->l = l; r->d = d; r->n = n; DV_COPY(r + 1, rg, n * sizeof *rg);
}
static void dv_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer s, VkImage d, VkImageLayout l, uint32_t n, const VkBufferImageCopy *rg) {
    if (!dv_enabled()) { DV_REAL(vkCmdCopyBufferToImage)(cb, s, d, l, n, rg); return; }
    struct { VkBuffer s; VkImage d; VkImageLayout l; uint32_t n; } *r = dv_alloc(DV_COPY_BUFFER_TO_IMAGE, cb, sizeof *r + n * sizeof *rg);
    r->s = s; r->d = d; r->l = l; r->n = n; DV_COPY(r + 1, rg, n * sizeof *rg);
}
static void dv_vkCmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *rg) {
    if (!dv_enabled()) { DV_REAL(vkCmdCopyImage)(cb, s, sl, d, dl, n, rg); return; }
    struct { VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; } *r = dv_alloc(DV_COPY_IMAGE, cb, sizeof *r + n * sizeof *rg);
    r->s = s; r->sl = sl; r->d = d; r->dl = dl; r->n = n; DV_COPY(r + 1, rg, n * sizeof *rg);
}
static void dv_vkCmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *rg, VkFilter f) {
    if (!dv_enabled()) { DV_REAL(vkCmdBlitImage)(cb, s, sl, d, dl, n, rg, f); return; }
    struct { VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; VkFilter f; } *r = dv_alloc(DV_BLIT_IMAGE, cb, sizeof *r + n * sizeof *rg);
    r->s = s; r->sl = sl; r->d = d; r->dl = dl; r->n = n; r->f = f; DV_COPY(r + 1, rg, n * sizeof *rg);
}
static void dv_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (!dv_enabled()) { DV_REAL(vkCmdBindPipeline)(cb, bp, p); return; }
    struct { VkPipelineBindPoint bp; VkPipeline p; } *r = dv_alloc(DV_BIND_PIPELINE, cb, sizeof *r); r->bp = bp; r->p = p;
}
static void dv_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *sets, uint32_t nd, const uint32_t *dyn) {
    if (!dv_enabled()) { DV_REAL(vkCmdBindDescriptorSets)(cb, bp, l, first, n, sets, nd, dyn); return; }
    struct { VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, nd; } *r = dv_alloc(DV_BIND_DESCRIPTOR_SETS, cb, sizeof *r + n * sizeof *sets + nd * sizeof *dyn);
    r->bp = bp; r->l = l; r->first = first; r->n = n; r->nd = nd; DV_COPY(r + 1, sets, n * sizeof *sets); DV_COPY((uint8_t *)(r + 1) + n * sizeof *sets, dyn, nd * sizeof *dyn);
}
static void dv_vkCmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits st, VkQueryPool p, uint32_t q) {
    if (!dv_enabled()) { DV_REAL(vkCmdWriteTimestamp)(cb, st, p, q); return; }
    struct { VkPipelineStageFlagBits st; VkQueryPool p; uint32_t q; } *r = dv_alloc(DV_WRITE_TIMESTAMP, cb, sizeof *r); r->st = st; r->p = p; r->q = q;
}
static void dv_vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool p, uint32_t first, uint32_t n) {
    if (!dv_enabled()) { DV_REAL(vkCmdResetQueryPool)(cb, p, first, n); return; }
    struct { VkQueryPool p; uint32_t first, n; } *r = dv_alloc(DV_RESET_QUERY_POOL, cb, sizeof *r); r->p = p; r->first = first; r->n = n;
}
static void dv_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (!dv_enabled()) { DV_REAL(vkCmdDispatch)(cb, x, y, z); return; }
    uint32_t *r = dv_alloc(DV_DISPATCH, cb, 12); r[0] = x; r[1] = y; r[2] = z;
}
static void dv_vkCmdClearColorImage(VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *rg) {
    if (!dv_enabled()) { DV_REAL(vkCmdClearColorImage)(cb, im, l, c, n, rg); return; }
    struct { VkImage im; VkImageLayout l; VkClearColorValue c; uint32_t n; } *r = dv_alloc(DV_CLEAR_COLOR_IMAGE, cb, sizeof *r + n * sizeof *rg);
    r->im = im; r->l = l; r->c = *c; r->n = n; DV_COPY(r + 1, rg, n * sizeof *rg);
}
static void dv_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearDepthStencilValue *c, uint32_t n, const VkImageSubresourceRange *rg) {
    if (!dv_enabled()) { DV_REAL(vkCmdClearDepthStencilImage)(cb, im, l, c, n, rg); return; }
    struct { VkImage im; VkImageLayout l; VkClearDepthStencilValue c; uint32_t n; } *r = dv_alloc(DV_CLEAR_DEPTH_STENCIL_IMAGE, cb, sizeof *r + n * sizeof *rg);
    r->im = im; r->l = l; r->c = *c; r->n = n; DV_COPY(r + 1, rg, n * sizeof *rg);
}
static void dv_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *rc) {
    if (!dv_enabled()) { DV_REAL(vkCmdClearAttachments)(cb, na, a, nr, rc); return; }
    struct { uint32_t na, nr; } *r = dv_alloc(DV_CLEAR_ATTACHMENTS, cb, sizeof *r + na * sizeof *a + nr * sizeof *rc);
    r->na = na; r->nr = nr; DV_COPY(r + 1, a, na * sizeof *a); DV_COPY((uint8_t *)(r + 1) + na * sizeof *a, rc, nr * sizeof *rc);
}
static void dv_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *v) {
    if (!dv_enabled()) { DV_REAL(vkCmdPushConstants)(cb, l, st, off, size, v); return; }
    struct { VkPipelineLayout l; VkShaderStageFlags st; uint32_t off, size; } *r = dv_alloc(DV_PUSH_CONSTANTS, cb, sizeof *r + size);
    r->l = l; r->st = st; r->off = off; r->size = size; DV_COPY(r + 1, v, size);
}
static void dv_vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size, uint32_t data) {
    if (!dv_enabled()) { DV_REAL(vkCmdFillBuffer)(cb, b, off, size, data); return; }
    struct { VkBuffer b; VkDeviceSize off, size; uint32_t data; } *r = dv_alloc(DV_FILL_BUFFER, cb, sizeof *r); r->b = b; r->off = off; r->size = size; r->data = data;
}
static void dv_vkCmdEndRenderPass(VkCommandBuffer cb) { if (!dv_enabled()) { DV_REAL(vkCmdEndRenderPass)(cb); return; } dv_alloc(DV_END_RENDER_PASS, cb, 0); }
static void dv_vkCmdEndQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q) {
    if (!dv_enabled()) { DV_REAL(vkCmdEndQuery)(cb, p, q); return; }
    struct { VkQueryPool p; uint32_t q; } *r = dv_alloc(DV_END_QUERY, cb, sizeof *r); r->p = p; r->q = q;
}
static void dv_vkCmdBeginQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f) {
    if (!dv_enabled()) { DV_REAL(vkCmdBeginQuery)(cb, p, q, f); return; }
    struct { VkQueryPool p; uint32_t q; VkQueryControlFlags f; } *r = dv_alloc(DV_BEGIN_QUERY, cb, sizeof *r); r->p = p; r->q = q; r->f = f;
}
static void dv_vkCmdCopyQueryPoolResults(VkCommandBuffer cb, VkQueryPool p, uint32_t first, uint32_t n, VkBuffer d, VkDeviceSize off, VkDeviceSize stride, VkQueryResultFlags f) {
    if (!dv_enabled()) { DV_REAL(vkCmdCopyQueryPoolResults)(cb, p, first, n, d, off, stride, f); return; }
    struct { VkQueryPool p; uint32_t first, n; VkBuffer d; VkDeviceSize off, stride; VkQueryResultFlags f; } *r = dv_alloc(DV_COPY_QUERY_POOL_RESULTS, cb, sizeof *r);
    r->p = p; r->first = first; r->n = n; r->d = d; r->off = off; r->stride = stride; r->f = f;
}
static void dv_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c) {
    if (!dv_enabled()) { DV_REAL(vkCmdBeginRenderPass)(cb, bi, c); return; }
    if (bi->pNext) die("Vulkan deferred render pass begin with a pNext chain");
    struct { VkRenderPassBeginInfo bi; VkSubpassContents c; } *r = dv_alloc(DV_BEGIN_RENDER_PASS, cb, sizeof *r + bi->clearValueCount * sizeof(VkClearValue));
    r->bi = *bi; r->c = c; DV_COPY(r + 1, bi->pClearValues, bi->clearValueCount * sizeof(VkClearValue));
}
static void dv_vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v) {
    if (!dv_enabled()) { DV_REAL(vkCmdSetViewport)(cb, first, n, v); return; }
    struct { uint32_t first, n; } *r = dv_alloc(DV_SET_VIEWPORT, cb, sizeof *r + n * sizeof *v); r->first = first; r->n = n; DV_COPY(r + 1, v, n * sizeof *v);
}
static void dv_vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *v) {
    if (!dv_enabled()) { DV_REAL(vkCmdSetScissor)(cb, first, n, v); return; }
    struct { uint32_t first, n; } *r = dv_alloc(DV_SET_SCISSOR, cb, sizeof *r + n * sizeof *v); r->first = first; r->n = n; DV_COPY(r + 1, v, n * sizeof *v);
}
#define DV_FACE_U32(name, op, real) static void name(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) { \
    if (!dv_enabled()) { DV_REAL(real)(cb, f, v); return; } uint32_t *r = dv_alloc(op, cb, 8); r[0] = f; r[1] = v; }
DV_FACE_U32(dv_vkCmdSetStencilWriteMask, DV_SET_STENCIL_WRITE_MASK, vkCmdSetStencilWriteMask)
DV_FACE_U32(dv_vkCmdSetStencilReference, DV_SET_STENCIL_REFERENCE, vkCmdSetStencilReference)
DV_FACE_U32(dv_vkCmdSetStencilCompareMask, DV_SET_STENCIL_COMPARE_MASK, vkCmdSetStencilCompareMask)
static void dv_vkCmdSetDepthBias(VkCommandBuffer cb, float c, float clamp, float slope) {
    if (!dv_enabled()) { DV_REAL(vkCmdSetDepthBias)(cb, c, clamp, slope); return; }
    float *r = dv_alloc(DV_SET_DEPTH_BIAS, cb, 12); r[0] = c; r[1] = clamp; r[2] = slope;
}
static void dv_vkCmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    if (!dv_enabled()) { DV_REAL(vkCmdSetBlendConstants)(cb, c); return; }
    float *r = dv_alloc(DV_SET_BLEND_CONSTANTS, cb, 16); memcpy(r, c, 16);
}
static void dv_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fi, int32_t vo, uint32_t finst) {
    if (!dv_enabled()) { DV_REAL(vkCmdDrawIndexed)(cb, ic, inst, fi, vo, finst); return; }
    uint32_t *r = dv_alloc(DV_DRAW_INDEXED, cb, 20); r[0] = ic; r[1] = inst; r[2] = fi; memcpy(&r[3], &vo, 4); r[4] = finst;
}
static void dv_vkCmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t inst, uint32_t fv, uint32_t finst) {
    if (!dv_enabled()) { DV_REAL(vkCmdDraw)(cb, vc, inst, fv, finst); return; }
    uint32_t *r = dv_alloc(DV_DRAW, cb, 16); r[0] = vc; r[1] = inst; r[2] = fv; r[3] = finst;
}
static void dv_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    if (!dv_enabled()) { DV_REAL(vkCmdBindVertexBuffers)(cb, first, n, b, o); return; }
    struct { uint32_t first, n; } *r = dv_alloc(DV_BIND_VERTEX_BUFFERS, cb, sizeof *r + n * (sizeof *b + sizeof *o));
    r->first = first; r->n = n; DV_COPY(r + 1, o, n * sizeof *o); DV_COPY((uint8_t *)(r + 1) + n * sizeof *o, b, n * sizeof *b);
}
static void dv_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    if (!dv_enabled()) { DV_REAL(vkCmdBindIndexBuffer)(cb, b, o, t); return; }
    struct { VkBuffer b; VkDeviceSize o; VkIndexType t; } *r = dv_alloc(DV_BIND_INDEX_BUFFER, cb, sizeof *r); r->b = b; r->o = o; r->t = t;
}
/* extended dynamic state: the function pointers are loaded at device creation (eds_*) */
#define DV_EDS_U32(name, op, ptr) static void name(VkCommandBuffer cb, uint32_t v) { \
    if (!dv_enabled()) { ptr(cb, v); return; } uint32_t *r = dv_alloc(op, cb, 4); r[0] = v; }
DV_EDS_U32(dv_eds_depth_test, DV_EDS_DEPTH_TEST, eds_depth_test)
DV_EDS_U32(dv_eds_depth_write, DV_EDS_DEPTH_WRITE, eds_depth_write)
DV_EDS_U32(dv_eds_depth_compare, DV_EDS_DEPTH_COMPARE, eds_depth_compare)
DV_EDS_U32(dv_eds_stencil_test, DV_EDS_STENCIL_TEST, eds_stencil_test)
DV_EDS_U32(dv_eds_cull, DV_EDS_CULL, eds_cull)
DV_EDS_U32(dv_eds_front, DV_EDS_FRONT, eds_front)
DV_EDS_U32(dv_eds_bias_enable, DV_EDS_BIAS_ENABLE, eds_bias_enable)
static void dv_eds_stencil_op(VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp fail, VkStencilOp pass, VkStencilOp zfail, VkCompareOp cmp) {
    if (!dv_enabled()) { eds_stencil_op(cb, f, fail, pass, zfail, cmp); return; }
    uint32_t *r = dv_alloc(DV_EDS_STENCIL_OP, cb, 20); r[0] = f; r[1] = fail; r[2] = pass; r[3] = zfail; r[4] = cmp;
}
/* command buffers and the queue */
static VkResult dv_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
    if (!dv_enabled()) return DV_REAL(vkBeginCommandBuffer)(cb, bi);
    if (bi->pNext || bi->pInheritanceInfo) die("Vulkan deferred begin of a secondary / chained command buffer");
    VkCommandBufferUsageFlags *r = dv_alloc(DV_BEGIN_COMMAND_BUFFER, cb, sizeof *r); *r = bi->flags; return VK_SUCCESS;
}
static VkResult dv_vkEndCommandBuffer(VkCommandBuffer cb) { if (!dv_enabled()) return DV_REAL(vkEndCommandBuffer)(cb); dv_alloc(DV_END_COMMAND_BUFFER, cb, 0); return VK_SUCCESS; }
static VkResult dv_vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f) {
    if (!dv_enabled()) return DV_REAL(vkResetCommandBuffer)(cb, f);
    VkCommandBufferResetFlags *r = dv_alloc(DV_RESET_COMMAND_BUFFER, cb, sizeof *r); *r = f; return VK_SUCCESS;
}
static VkResult dv_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence fence) {
    if (!dv_enabled()) return DV_REAL(vkQueueSubmit)(q, n, s, fence);
    size_t bytes = 0;
    for (uint32_t i = 0; i < n; i++) { if (s[i].pNext) die("Vulkan deferred submit with a pNext chain");
        bytes += sizeof(VkSubmitInfo) + s[i].waitSemaphoreCount * (sizeof(VkSemaphore) + sizeof(VkPipelineStageFlags)) + s[i].commandBufferCount * sizeof(VkCommandBuffer) + s[i].signalSemaphoreCount * sizeof(VkSemaphore); }
    struct { VkQueue q; VkFence fence; uint32_t n; } *r = dv_alloc(DV_QUEUE_SUBMIT, VK_NULL_HANDLE, sizeof *r + bytes); r->q = q; r->fence = fence; r->n = n;
    uint8_t *t = (uint8_t *)(r + 1);
    for (uint32_t i = 0; i < n; i++) {
        memcpy(t, &s[i], sizeof s[i]); t += sizeof s[i];
        DV_COPY(t, s[i].pWaitSemaphores, s[i].waitSemaphoreCount * sizeof(VkSemaphore)); t += s[i].waitSemaphoreCount * sizeof(VkSemaphore);
        DV_COPY(t, s[i].pWaitDstStageMask, s[i].waitSemaphoreCount * sizeof(VkPipelineStageFlags)); t += s[i].waitSemaphoreCount * sizeof(VkPipelineStageFlags);
        DV_COPY(t, s[i].pCommandBuffers, s[i].commandBufferCount * sizeof(VkCommandBuffer)); t += s[i].commandBufferCount * sizeof(VkCommandBuffer);
        DV_COPY(t, s[i].pSignalSemaphores, s[i].signalSemaphoreCount * sizeof(VkSemaphore)); t += s[i].signalSemaphoreCount * sizeof(VkSemaphore);
    }
    dv_maybe_publish(1); return VK_SUCCESS;
}

static void dv_replay(const uint8_t *p, const uint8_t *end) {
    while (p < end) {
        const dv_hdr *h = (const dv_hdr *)p; const uint8_t *a = (const uint8_t *)(h + 1); VkCommandBuffer cb = h->cb;
        switch (h->op) {
        case DV_PIPELINE_BARRIER: { const struct { VkPipelineStageFlags src, dst; VkDependencyFlags dep; uint32_t nm, nb, ni; } *r = (const void *)a; const uint8_t *t = (const uint8_t *)(r + 1);
            const VkMemoryBarrier *m = (const void *)t; const VkBufferMemoryBarrier *b = (const void *)(t + r->nm * sizeof *m); const VkImageMemoryBarrier *im = (const void *)((const uint8_t *)b + r->nb * sizeof *b);
            DV_REAL(vkCmdPipelineBarrier)(cb, r->src, r->dst, r->dep, r->nm, r->nm ? m : NULL, r->nb, r->nb ? b : NULL, r->ni, r->ni ? im : NULL); break; }
        case DV_COPY_BUFFER: { const struct { VkBuffer s, d; uint32_t n; } *r = (const void *)a; DV_REAL(vkCmdCopyBuffer)(cb, r->s, r->d, r->n, (const void *)(r + 1)); break; }
        case DV_COPY_IMAGE_TO_BUFFER: { const struct { VkImage s; VkImageLayout l; VkBuffer d; uint32_t n; } *r = (const void *)a; DV_REAL(vkCmdCopyImageToBuffer)(cb, r->s, r->l, r->d, r->n, (const void *)(r + 1)); break; }
        case DV_COPY_BUFFER_TO_IMAGE: { const struct { VkBuffer s; VkImage d; VkImageLayout l; uint32_t n; } *r = (const void *)a; DV_REAL(vkCmdCopyBufferToImage)(cb, r->s, r->d, r->l, r->n, (const void *)(r + 1)); break; }
        case DV_COPY_IMAGE: { const struct { VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; } *r = (const void *)a; DV_REAL(vkCmdCopyImage)(cb, r->s, r->sl, r->d, r->dl, r->n, (const void *)(r + 1)); break; }
        case DV_BLIT_IMAGE: { const struct { VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; VkFilter f; } *r = (const void *)a; DV_REAL(vkCmdBlitImage)(cb, r->s, r->sl, r->d, r->dl, r->n, (const void *)(r + 1), r->f); break; }
        case DV_BIND_PIPELINE: { const struct { VkPipelineBindPoint bp; VkPipeline p; } *r = (const void *)a; DV_REAL(vkCmdBindPipeline)(cb, r->bp, r->p); break; }
        case DV_BIND_DESCRIPTOR_SETS: { const struct { VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, nd; } *r = (const void *)a; const VkDescriptorSet *sets = (const void *)(r + 1);
            DV_REAL(vkCmdBindDescriptorSets)(cb, r->bp, r->l, r->first, r->n, sets, r->nd, r->nd ? (const uint32_t *)(sets + r->n) : NULL); break; }
        case DV_WRITE_TIMESTAMP: { const struct { VkPipelineStageFlagBits st; VkQueryPool p; uint32_t q; } *r = (const void *)a; DV_REAL(vkCmdWriteTimestamp)(cb, r->st, r->p, r->q); break; }
        case DV_RESET_QUERY_POOL: { const struct { VkQueryPool p; uint32_t first, n; } *r = (const void *)a; DV_REAL(vkCmdResetQueryPool)(cb, r->p, r->first, r->n); break; }
        case DV_DISPATCH: { const uint32_t *r = (const void *)a; DV_REAL(vkCmdDispatch)(cb, r[0], r[1], r[2]); break; }
        case DV_CLEAR_COLOR_IMAGE: { const struct { VkImage im; VkImageLayout l; VkClearColorValue c; uint32_t n; } *r = (const void *)a; DV_REAL(vkCmdClearColorImage)(cb, r->im, r->l, &r->c, r->n, (const void *)(r + 1)); break; }
        case DV_CLEAR_DEPTH_STENCIL_IMAGE: { const struct { VkImage im; VkImageLayout l; VkClearDepthStencilValue c; uint32_t n; } *r = (const void *)a; DV_REAL(vkCmdClearDepthStencilImage)(cb, r->im, r->l, &r->c, r->n, (const void *)(r + 1)); break; }
        case DV_CLEAR_ATTACHMENTS: { const struct { uint32_t na, nr; } *r = (const void *)a; const VkClearAttachment *att = (const void *)(r + 1);
            DV_REAL(vkCmdClearAttachments)(cb, r->na, att, r->nr, (const void *)(att + r->na)); break; }
        case DV_PUSH_CONSTANTS: { const struct { VkPipelineLayout l; VkShaderStageFlags st; uint32_t off, size; } *r = (const void *)a; DV_REAL(vkCmdPushConstants)(cb, r->l, r->st, r->off, r->size, r + 1); break; }
        case DV_FILL_BUFFER: { const struct { VkBuffer b; VkDeviceSize off, size; uint32_t data; } *r = (const void *)a; DV_REAL(vkCmdFillBuffer)(cb, r->b, r->off, r->size, r->data); break; }
        case DV_END_RENDER_PASS: DV_REAL(vkCmdEndRenderPass)(cb); break;
        case DV_END_QUERY: { const struct { VkQueryPool p; uint32_t q; } *r = (const void *)a; DV_REAL(vkCmdEndQuery)(cb, r->p, r->q); break; }
        case DV_BEGIN_QUERY: { const struct { VkQueryPool p; uint32_t q; VkQueryControlFlags f; } *r = (const void *)a; DV_REAL(vkCmdBeginQuery)(cb, r->p, r->q, r->f); break; }
        case DV_COPY_QUERY_POOL_RESULTS: { const struct { VkQueryPool p; uint32_t first, n; VkBuffer d; VkDeviceSize off, stride; VkQueryResultFlags f; } *r = (const void *)a;
            DV_REAL(vkCmdCopyQueryPoolResults)(cb, r->p, r->first, r->n, r->d, r->off, r->stride, r->f); break; }
        case DV_BEGIN_RENDER_PASS: { const struct { VkRenderPassBeginInfo bi; VkSubpassContents c; } *r = (const void *)a; VkRenderPassBeginInfo bi = r->bi;
            bi.pClearValues = bi.clearValueCount ? (const VkClearValue *)(r + 1) : NULL; DV_REAL(vkCmdBeginRenderPass)(cb, &bi, r->c); break; }
        case DV_SET_VIEWPORT: { const struct { uint32_t first, n; } *r = (const void *)a; DV_REAL(vkCmdSetViewport)(cb, r->first, r->n, (const void *)(r + 1)); break; }
        case DV_SET_SCISSOR: { const struct { uint32_t first, n; } *r = (const void *)a; DV_REAL(vkCmdSetScissor)(cb, r->first, r->n, (const void *)(r + 1)); break; }
        case DV_SET_STENCIL_WRITE_MASK: { const uint32_t *r = (const void *)a; DV_REAL(vkCmdSetStencilWriteMask)(cb, r[0], r[1]); break; }
        case DV_SET_STENCIL_REFERENCE: { const uint32_t *r = (const void *)a; DV_REAL(vkCmdSetStencilReference)(cb, r[0], r[1]); break; }
        case DV_SET_STENCIL_COMPARE_MASK: { const uint32_t *r = (const void *)a; DV_REAL(vkCmdSetStencilCompareMask)(cb, r[0], r[1]); break; }
        case DV_SET_DEPTH_BIAS: { const float *r = (const void *)a; DV_REAL(vkCmdSetDepthBias)(cb, r[0], r[1], r[2]); break; }
        case DV_SET_BLEND_CONSTANTS: DV_REAL(vkCmdSetBlendConstants)(cb, (const float *)a); break;
        case DV_DRAW_INDEXED: { const uint32_t *r = (const void *)a; int32_t vo; memcpy(&vo, &r[3], 4); DV_REAL(vkCmdDrawIndexed)(cb, r[0], r[1], r[2], vo, r[4]); break; }
        case DV_DRAW: { const uint32_t *r = (const void *)a; DV_REAL(vkCmdDraw)(cb, r[0], r[1], r[2], r[3]); break; }
        case DV_BIND_VERTEX_BUFFERS: { const struct { uint32_t first, n; } *r = (const void *)a; const VkDeviceSize *o = (const void *)(r + 1);
            DV_REAL(vkCmdBindVertexBuffers)(cb, r->first, r->n, (const VkBuffer *)(o + r->n), o); break; }
        case DV_BIND_INDEX_BUFFER: { const struct { VkBuffer b; VkDeviceSize o; VkIndexType t; } *r = (const void *)a; DV_REAL(vkCmdBindIndexBuffer)(cb, r->b, r->o, r->t); break; }
        case DV_EDS_DEPTH_TEST: eds_depth_test(cb, *(const uint32_t *)a); break;
        case DV_EDS_DEPTH_WRITE: eds_depth_write(cb, *(const uint32_t *)a); break;
        case DV_EDS_DEPTH_COMPARE: eds_depth_compare(cb, *(const uint32_t *)a); break;
        case DV_EDS_STENCIL_TEST: eds_stencil_test(cb, *(const uint32_t *)a); break;
        case DV_EDS_CULL: eds_cull(cb, *(const uint32_t *)a); break;
        case DV_EDS_FRONT: eds_front(cb, *(const uint32_t *)a); break;
        case DV_EDS_BIAS_ENABLE: eds_bias_enable(cb, *(const uint32_t *)a); break;
        case DV_EDS_STENCIL_OP: { const uint32_t *r = (const void *)a; eds_stencil_op(cb, r[0], r[1], r[2], r[3], r[4]); break; }
        case DV_BEGIN_COMMAND_BUFFER: { VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = *(const VkCommandBufferUsageFlags *)a};
            VK_CHECK(DV_REAL(vkBeginCommandBuffer)(cb, &bi)); break; }
        case DV_END_COMMAND_BUFFER: VK_CHECK(DV_REAL(vkEndCommandBuffer)(cb)); break;
        case DV_RESET_COMMAND_BUFFER: VK_CHECK(DV_REAL(vkResetCommandBuffer)(cb, *(const VkCommandBufferResetFlags *)a)); break;
        case DV_QUEUE_SUBMIT: { const struct { VkQueue q; VkFence fence; uint32_t n; } *r = (const void *)a; const uint8_t *t = (const uint8_t *)(r + 1);
            VkSubmitInfo s[8]; if (r->n > 8) die("Vulkan deferred submit count %u", r->n);
            for (uint32_t i = 0; i < r->n; i++) {
                memcpy(&s[i], t, sizeof s[i]); t += sizeof s[i];
                s[i].pWaitSemaphores = (const void *)t; t += s[i].waitSemaphoreCount * sizeof(VkSemaphore);
                s[i].pWaitDstStageMask = (const void *)t; t += s[i].waitSemaphoreCount * sizeof(VkPipelineStageFlags);
                s[i].pCommandBuffers = (const void *)t; t += s[i].commandBufferCount * sizeof(VkCommandBuffer);
                s[i].pSignalSemaphores = (const void *)t; t += s[i].signalSemaphoreCount * sizeof(VkSemaphore);
            }
            VK_CHECK(DV_REAL(vkQueueSubmit)(r->q, r->n, s, r->fence)); break; }
        default: die("Vulkan deferred record op %u", h->op);
        }
        p += h->size;
    }
}

/* route this file's calls through the recorder */
#undef vkCmdPipelineBarrier
#define vkCmdPipelineBarrier dv_vkCmdPipelineBarrier
#undef vkCmdCopyBuffer
#define vkCmdCopyBuffer dv_vkCmdCopyBuffer
#undef vkCmdCopyImageToBuffer
#define vkCmdCopyImageToBuffer dv_vkCmdCopyImageToBuffer
#undef vkCmdCopyBufferToImage
#define vkCmdCopyBufferToImage dv_vkCmdCopyBufferToImage
#undef vkCmdCopyImage
#define vkCmdCopyImage dv_vkCmdCopyImage
#undef vkCmdBlitImage
#define vkCmdBlitImage dv_vkCmdBlitImage
#undef vkCmdBindPipeline
#define vkCmdBindPipeline dv_vkCmdBindPipeline
#undef vkCmdBindDescriptorSets
#define vkCmdBindDescriptorSets dv_vkCmdBindDescriptorSets
#undef vkCmdWriteTimestamp
#define vkCmdWriteTimestamp dv_vkCmdWriteTimestamp
#undef vkCmdResetQueryPool
#define vkCmdResetQueryPool dv_vkCmdResetQueryPool
#undef vkCmdDispatch
#define vkCmdDispatch dv_vkCmdDispatch
#undef vkCmdClearColorImage
#define vkCmdClearColorImage dv_vkCmdClearColorImage
#undef vkCmdClearDepthStencilImage
#define vkCmdClearDepthStencilImage dv_vkCmdClearDepthStencilImage
#undef vkCmdClearAttachments
#define vkCmdClearAttachments dv_vkCmdClearAttachments
#undef vkCmdPushConstants
#define vkCmdPushConstants dv_vkCmdPushConstants
#undef vkCmdFillBuffer
#define vkCmdFillBuffer dv_vkCmdFillBuffer
#undef vkCmdEndRenderPass
#define vkCmdEndRenderPass dv_vkCmdEndRenderPass
#undef vkCmdEndQuery
#define vkCmdEndQuery dv_vkCmdEndQuery
#undef vkCmdBeginQuery
#define vkCmdBeginQuery dv_vkCmdBeginQuery
#undef vkCmdCopyQueryPoolResults
#define vkCmdCopyQueryPoolResults dv_vkCmdCopyQueryPoolResults
#undef vkCmdBeginRenderPass
#define vkCmdBeginRenderPass dv_vkCmdBeginRenderPass
#undef vkCmdSetViewport
#define vkCmdSetViewport dv_vkCmdSetViewport
#undef vkCmdSetScissor
#define vkCmdSetScissor dv_vkCmdSetScissor
#undef vkCmdSetStencilWriteMask
#define vkCmdSetStencilWriteMask dv_vkCmdSetStencilWriteMask
#undef vkCmdSetStencilReference
#define vkCmdSetStencilReference dv_vkCmdSetStencilReference
#undef vkCmdSetStencilCompareMask
#define vkCmdSetStencilCompareMask dv_vkCmdSetStencilCompareMask
#undef vkCmdSetDepthBias
#define vkCmdSetDepthBias dv_vkCmdSetDepthBias
#undef vkCmdSetBlendConstants
#define vkCmdSetBlendConstants dv_vkCmdSetBlendConstants
#undef vkCmdDrawIndexed
#define vkCmdDrawIndexed dv_vkCmdDrawIndexed
#undef vkCmdDraw
#define vkCmdDraw dv_vkCmdDraw
#undef vkCmdBindVertexBuffers
#define vkCmdBindVertexBuffers dv_vkCmdBindVertexBuffers
#undef vkCmdBindIndexBuffer
#define vkCmdBindIndexBuffer dv_vkCmdBindIndexBuffer
#undef vkBeginCommandBuffer
#define vkBeginCommandBuffer dv_vkBeginCommandBuffer
#undef vkEndCommandBuffer
#define vkEndCommandBuffer dv_vkEndCommandBuffer
#undef vkResetCommandBuffer
#define vkResetCommandBuffer dv_vkResetCommandBuffer
#undef vkQueueSubmit
#define vkQueueSubmit dv_vkQueueSubmit
/* the queue / device as a whole: the recorded work first */
#undef vkQueuePresentKHR
#define vkQueuePresentKHR(...) (dv_drain(), (vk_api.vkQueuePresentKHR)(__VA_ARGS__))
#undef vkQueueWaitIdle
#define vkQueueWaitIdle(...) (dv_drain(), (vk_api.vkQueueWaitIdle)(__VA_ARGS__))
#undef vkDeviceWaitIdle
#define vkDeviceWaitIdle(...) (dv_drain(), (vk_api.vkDeviceWaitIdle)(__VA_ARGS__))
/* the command pool is shared with the buffers being replayed: growing or shrinking it waits for the worker (rare) */
#undef vkAllocateCommandBuffers
#define vkAllocateCommandBuffers(...) (dv_drain(), (vk_api.vkAllocateCommandBuffers)(__VA_ARGS__))
#undef vkFreeCommandBuffers
#define vkFreeCommandBuffers(...) (dv_drain(), (vk_api.vkFreeCommandBuffers)(__VA_ARGS__))

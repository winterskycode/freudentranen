/* TOTK_VK_DRAWSTATS=1: fragment-shader invocations of every draw (pipeline statistics query), compared per draw identity
   (vertex shader, fragment shader, ordinal of that pair in the frame) with its running average. A draw that suddenly shades
   far more than usual is logged as [drawstats] spike: the one-frame flicker shapes are found without a capture. */
#define DRAWSTAT_CAPACITY 8192
typedef struct { const char *vs, *fs; uint32_t ordinal, count, instances, prim; uint64_t frame; } drawstat_info;
static struct { VkQueryPool pool; drawstat_info *info; uint32_t used; } drawstat_slots[DM_VK_BATCH_SLOTS];
/* -1 unknown, 0 off, 1 on */
static int drawstat_state = -1;
static int drawstat_on(void) {
    if (drawstat_state < 0) { const char *e = getenv("TOTK_VK_DRAWSTATS"); drawstat_state = e && atoi(e) > 0; }
    return drawstat_state;
}
static void drawstat_batch_begin(unsigned slot, VkCommandBuffer cb) {
    if (!drawstat_on()) return;
    if (!gpu_features.pipelineStatisticsQuery) { LOG("[drawstats] pipelineStatisticsQuery unsupported: off"); drawstat_state = 0; return; }
    if (!drawstat_slots[slot].pool) {
        VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS,
            .queryCount = DRAWSTAT_CAPACITY, .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT};
        VK_CHECK(vkCreateQueryPool(device, &info, NULL, &drawstat_slots[slot].pool));
        drawstat_slots[slot].info = calloc(DRAWSTAT_CAPACITY, sizeof *drawstat_slots[slot].info);
    }
    vkCmdResetQueryPool(cb, drawstat_slots[slot].pool, 0, DRAWSTAT_CAPACITY); drawstat_slots[slot].used = 0;
}
/* ordinal of a (vs, fs) pair within the current frame (recording thread only) */
static uint32_t drawstat_ordinal(const char *vs, const char *fs) {
    static struct { const char *vs, *fs; uint32_t n; uint64_t frame; } table[4096];
    uintptr_t h = ((uintptr_t)vs * 31u + (uintptr_t)fs) * 0x9E3779B97F4A7C15ull >> 52;
    for (unsigned i = 0; i < 4096; i++) {
        unsigned k = (h + i) & 4095;
        if (table[k].frame != presents || !table[k].vs) { table[k].vs = vs; table[k].fs = fs; table[k].n = 0; table[k].frame = presents; }
        if (table[k].vs == vs && table[k].fs == fs) return table[k].n++;
    }
    return 0;
}
static int drawstat_draw_begin(unsigned slot, VkCommandBuffer cb, const char *vs, const char *fs, uint32_t count, uint32_t instances, uint32_t prim) {
    if (drawstat_state <= 0 || !drawstat_slots[slot].pool || drawstat_slots[slot].used >= DRAWSTAT_CAPACITY) return -1;
    uint32_t q = drawstat_slots[slot].used++;
    drawstat_slots[slot].info[q] = (drawstat_info){vs, fs, drawstat_ordinal(vs, fs), count, instances, prim, presents};
    vkCmdBeginQuery(cb, drawstat_slots[slot].pool, q, 0);
    return (int)q;
}
static void drawstat_draw_end(unsigned slot, VkCommandBuffer cb, int q) { if (q >= 0) vkCmdEndQuery(cb, drawstat_slots[slot].pool, (uint32_t)q); }
/* completion worker: after the batch fence */
static void drawstat_batch_done(unsigned slot) {
    if (drawstat_state <= 0 || !drawstat_slots[slot].pool || !drawstat_slots[slot].used) return;
    static uint64_t results[DRAWSTAT_CAPACITY];
    static struct { const char *vs, *fs; uint32_t ordinal, seen; double avg; } hist[65536];
    static uint64_t spikes, draws, last_report;
    uint32_t n = drawstat_slots[slot].used;
    if (vkGetQueryPoolResults(device, drawstat_slots[slot].pool, 0, n, sizeof results, results, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    for (uint32_t i = 0; i < n; i++) {
        const drawstat_info *d = &drawstat_slots[slot].info[i]; double v = (double)results[i]; draws++;
        uintptr_t h = (((uintptr_t)d->vs * 31u + (uintptr_t)d->fs) * 131u + d->ordinal) * 0x9E3779B97F4A7C15ull >> 48;
        unsigned k = h & 65535, probes = 0;
        while (hist[k].vs && !(hist[k].vs == d->vs && hist[k].fs == d->fs && hist[k].ordinal == d->ordinal) && probes++ < 64) k = (k + 1) & 65535;
        if (probes >= 64) continue;
        if (!hist[k].vs) { hist[k].vs = d->vs; hist[k].fs = d->fs; hist[k].ordinal = d->ordinal; hist[k].avg = v; }
        else if (hist[k].seen >= 30 && v > 4 * hist[k].avg + 20000) {
            spikes++;
            if (spikes <= 400) LOG("[drawstats] spike frame=%llu vs=%s fs=%s ordinal=%u fragments=%.0f average=%.0f (x%.1f) count=%u instances=%u prim=%u seen=%u",
                (unsigned long long)d->frame, d->vs, d->fs ? d->fs : "-", d->ordinal, v, hist[k].avg, v / (hist[k].avg + 1), d->count, d->instances, d->prim, hist[k].seen);
        }
        hist[k].avg = hist[k].avg * 0.9 + v * 0.1; hist[k].seen++;
    }
    if (presents >= last_report + 600) { last_report = presents; LOG("[drawstats] frame=%llu draws=%llu spikes=%llu", (unsigned long long)presents, (unsigned long long)draws, (unsigned long long)spikes); }
}

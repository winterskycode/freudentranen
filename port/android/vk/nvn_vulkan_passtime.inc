/* TOTK_VK_PASSTIME=1: GPU time of every render pass (timestamps before vkCmdBeginRenderPass and after vkCmdEndRenderPass),
   grouped by pass shape (size, attachment count, first format, draws); every 300 presents the shapes costing the most GPU
   time per frame are logged as [passtime]. TOTK_VK_PASSTIME=2 also dumps one frame in 600 pass by pass, in order, as
   [passseq]: when the pass's recording started (ms after the frame's first pass), the translation thread's wall time
   recording it, its GPU time, draws, size and first fragment shader: which passes the GPU eats faster than they are
   translated. */
#define PASSTIME_CAPACITY 2048
/* fs: the first draw's fragment shader */
typedef struct { uint32_t width, height, attachments, format, draws; const char *fs; uint32_t written; } passtime_key;
static struct { VkQueryPool pool; passtime_key keys[PASSTIME_CAPACITY / 2]; uint32_t used; } passtime_slots[DM_VK_BATCH_SLOTS];
static int passtime_state = -1;
static int passtime_on(void) { if (passtime_state < 0) { const char *e = getenv("TOTK_VK_PASSTIME"); passtime_state = e && atoi(e) > 0 ? atoi(e) : 0; } return passtime_state; }
/* draws in the pass being recorded */
static uint32_t passtime_draws; static const char *passtime_fs; static uint32_t passtime_written;
/* query pair index of the pass being recorded, -1 none */
static int passtime_open = -1;
/* TOTK_VK_PASSTIME=2 */
static struct { uint64_t t0, t1, frame; } passtime_cpu[DM_VK_BATCH_SLOTS][PASSTIME_CAPACITY / 2];
/* the frame being dumped, the CPU time of its first pass */
static uint64_t passseq_frame, passseq_t0;
static void passtime_batch_begin(unsigned slot, VkCommandBuffer cb) {
    if (!passtime_on()) return;
    if (!passtime_slots[slot].pool) { VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = PASSTIME_CAPACITY};
        VK_CHECK(vkCreateQueryPool(device, &info, NULL, &passtime_slots[slot].pool)); }
    vkCmdResetQueryPool(cb, passtime_slots[slot].pool, 0, PASSTIME_CAPACITY); passtime_slots[slot].used = 0; passtime_open = -1;
}
static void passtime_pass_begin(unsigned slot, VkCommandBuffer cb, passtime_key key) {
    if (passtime_state <= 0 || !passtime_slots[slot].pool || passtime_slots[slot].used >= PASSTIME_CAPACITY / 2) { passtime_open = -1; return; }
    uint32_t q = passtime_slots[slot].used++; passtime_slots[slot].keys[q] = key; passtime_open = (int)q;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, passtime_slots[slot].pool, 2 * q);
    if (passtime_state > 1) { passtime_cpu[slot][q].t0 = dm_android_clock_ns(CLOCK_MONOTONIC); passtime_cpu[slot][q].t1 = 0; passtime_cpu[slot][q].frame = presents; }
}
static void passtime_pass_end(unsigned slot, VkCommandBuffer cb, uint32_t draws) {
    if (passtime_open < 0) return;
    passtime_slots[slot].keys[passtime_open].draws = draws; passtime_slots[slot].keys[passtime_open].fs = passtime_fs; passtime_slots[slot].keys[passtime_open].written = passtime_written; passtime_fs = NULL; passtime_written = 0;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, passtime_slots[slot].pool, 2 * (uint32_t)passtime_open + 1);
    if (passtime_state > 1) passtime_cpu[slot][passtime_open].t1 = dm_android_clock_ns(CLOCK_MONOTONIC);
    passtime_open = -1;
}
static void passtime_batch_done(unsigned slot) {
    if (passtime_state <= 0 || !passtime_slots[slot].pool || !passtime_slots[slot].used) return;
    static uint64_t ts[PASSTIME_CAPACITY];
    static struct { passtime_key key; double ms; uint64_t count; } table[4096]; static unsigned n; static uint64_t last; static double untabled;
    uint32_t pairs = passtime_slots[slot].used;
    if (vkGetQueryPoolResults(device, passtime_slots[slot].pool, 0, 2 * pairs, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    /* totals by kind: size class x draw-count bucket (all passes, not only the top shapes) */
    static double kind_ms[7][5]; static uint64_t kind_n[7][5];
    for (uint32_t i = 0; i < pairs; i++) {
        double ms = (double)timestamp_delta(ts[2 * i], ts[2 * i + 1], gpu_timestamp_bits) * gpu_timestamp_period / 1e6;
        { const passtime_key *pk = &passtime_slots[slot].keys[i]; uint32_t w = pk->width, h = pk->height, d = pk->draws;
          int sc = !w && !h ? 6 : w == h && w >= 256 ? 3 : w >= 1200 ? 0 : w >= 600 ? 1 : w >= 300 ? 2 : pk->attachments == 0 ? 5 : 4;
          int db = d <= 1 ? 0 : d <= 4 ? 1 : d <= 16 ? 2 : d <= 64 ? 3 : 4; kind_ms[sc][db] += ms; kind_n[sc][db]++; }
        /* draw-count bucket */
        passtime_key k = passtime_slots[slot].keys[i]; k.draws = k.draws > 64 ? 64 : k.draws > 16 ? 16 : k.draws > 4 ? 4 : k.draws;
        unsigned j = 0; while (j < n && memcmp(&table[j].key, &k, sizeof k)) j++;
        if (j == n) { if (n == 4096) { untabled += ms; continue; } table[n].key = k; table[n].ms = 0; table[n].count = 0; n++; }
        table[j].ms += ms; table[j].count++;
        /* TOTK_PASSSEQ_FS=<fs key>: every such pass's recording window, every frame (to match CPU samples to it) */
        { static const char *only = (const char *)-1; if (only == (const char *)-1) only = getenv("TOTK_PASSSEQ_FS");
          const passtime_key *pk = &passtime_slots[slot].keys[i];
          if (only && passtime_state > 1 && pk->fs && !strncmp(pk->fs, only, strlen(only)) && passtime_cpu[slot][i].t0)
              LOG("[passwin] %llu %llu", (unsigned long long)passtime_cpu[slot][i].t0, (unsigned long long)passtime_cpu[slot][i].t1); }
        /* the pass sequence of one frame in 600 */
        if (passtime_state > 1) {
            uint64_t f = passtime_cpu[slot][i].frame, t0 = passtime_cpu[slot][i].t0, t1 = passtime_cpu[slot][i].t1;
            if (!passseq_frame && f >= 600 && f % 600 == 0) { passseq_frame = f; passseq_t0 = t0; }
            if (passseq_frame && f == passseq_frame && t0) {
                const passtime_key *pk = &passtime_slots[slot].keys[i];
                LOG("[passseq] frame=%llu +%.3f ms cpu %.3f gpu %.3f draws %u %ux%u att %u fs %s t0=%llu t1=%llu", (unsigned long long)f, (t0 - passseq_t0) / 1e6,
                    t1 > t0 ? (t1 - t0) / 1e6 : 0.0, ms, pk->draws, pk->width, pk->height, pk->attachments, pk->fs ? pk->fs : "-", (unsigned long long)t0, (unsigned long long)t1);
            } else if (passseq_frame && f > passseq_frame) passseq_frame = 0;
        }
    }
    if (presents >= last + 300) { uint64_t frames = last ? presents - last : 300; last = presents; double total = 0;
        for (unsigned j = 0; j < n; j++) total += table[j].ms; total += untabled; untabled = 0;
        { static double g0; LOG("[passtime] frame=%llu render passes %.2f ms/frame in %u shapes; batch GPU spans %.2f ms/frame (the difference: barriers, copies, clears and dispatches outside passes, and feed stalls inside batches)", (unsigned long long)presents, total / frames, n, (gpu_total_ns - g0) / (frames * 1e6)); g0 = gpu_total_ns; dv_census_report(frames); }
        { static const char *sn[7] = {"full", "half", "quarter", "square", "small", "no-target", "dispatch"}, *dn[5] = {"1", "2-4", "5-16", "17-64", "65+"};
          for (int sc = 0; sc < 7; sc++) { char line[256]; int k = 0; double row = 0;
              for (int db = 0; db < 5; db++) { row += kind_ms[sc][db]; if (kind_n[sc][db]) k += snprintf(line + k, sizeof line - k, "  %s draws: %.2f ms (%.1f passes)", dn[db], kind_ms[sc][db] / frames, (double)kind_n[sc][db] / frames); }
              if (row > 0) LOG("[passtime-kind] %-9s %.2f ms/frame:%s", sn[sc], row / frames, line); }
          memset(kind_ms, 0, sizeof kind_ms); memset(kind_n, 0, sizeof kind_n); }
        for (int shown = 0; shown < 24; shown++) { unsigned best = n; for (unsigned j = 0; j < n; j++) if (table[j].count && (best == n || table[j].ms > table[best].ms)) best = j;
            if (best == n) break;
            LOG("[passtime]   %ux%u att=%u fmt=%u draws<=%u fs=%s written=%#x: %.3f ms/frame, %.1f passes/frame", table[best].key.width, table[best].key.height, table[best].key.attachments,
                table[best].key.format, table[best].key.draws, table[best].key.fs ? table[best].key.fs : "-", table[best].key.written, table[best].ms / frames, (double)table[best].count / frames);
            table[best].count = 0; }
        n = 0; }
}

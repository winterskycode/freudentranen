/* Present pacing with frame generation (VK_GOOGLE_display_timing). The generated picture and the real one are presented
   back to back; in FIFO each takes one refresh, so at 120 Hz with the game at 30 the generated picture is on screen for
   8.3 ms and the real one for 25 ms: halfway positions shown for a quarter of the time, a visible stutter on fast pans
   (the glowing door in the Room of Awakening). The display rate is only requested (TOTK_DISPLAY_HZ); other layers can
   keep the panel at 120 Hz. Here each picture gets a "not before" time: with n generated pictures (1, or up to 3 with
   LSFG) each comes 1 / (n + 1) of a game frame after the one before, and the real one the same after the last, on the
   refresh grid learnt from past present times. A late game frame is shown as soon as possible instead. TOTK_PACING=1
   turns it on; it is off by default. */
/* VK_GOOGLE_display_timing listed (enabled at device creation) */
static int pace_ext;
/* totk_timeline.c */
void totk_tl_at(int kind, uint64_t t_ns, uint64_t a, uint64_t b) __attribute__((weak));
/* TL_SHOWN: a picture reached the screen */
#include "totk_timeline.h"
static PFN_vkGetRefreshCycleDurationGOOGLE pace_get_refresh;
static PFN_vkGetPastPresentationTimingGOOGLE pace_get_past;
static VkSwapchainKHR pace_chain;
/* ns; anchor: a past refresh */
static uint64_t pace_refresh, pace_anchor, pace_last_target;
/* From a present until its picture is drawn (its GPU work done: a high estimate), and from then until the screen shows it
   when nothing holds it back. A picture that is not ready before the refresh the next one is due at is dropped, so the plan
   leads by both. */
static uint64_t pace_gpu = 8000000, pace_comp;
static uint32_t pace_id, pace_seen_id, pace_ready_id;
/* the next present's "not before" time (0: as soon as possible, -1: no timing) */
static int64_t pace_desired = -1;
/* by present id % 64: submit time, for the latency estimate */
static uint64_t pace_submit[64];
/* by present id % 64: the command buffer that draws it, and when the GPU finished it (0 not known) */
static uint64_t pace_ticket[64], pace_done[64];
/* by command buffer ticket % 1024, from the completion thread (nvn_vulkan_async.inc) */
static _Atomic uint64_t pace_fence_ticket[1024], pace_fence_ns[1024];
static void pace_fence_passed(uint64_t ticket, uint64_t ns) { atomic_store(&pace_fence_ns[ticket % 1024], ns); atomic_store(&pace_fence_ticket[ticket % 1024], ticket); }
static uint64_t pace_frames, pace_late, pace_missed;

/* frame generation fill mode (nvn_vulkan_fgfill.inc): real frames go on the frame cap's grid too, so the pictures that
   fill missed refreshes and the real ones share one schedule; it turns pacing on */
static int fg_fill;
static int fg_enabled(void); static int fg_shown_hz(void); static int lsfg_enabled(void);
static int pace_on(void) {
    /* every frame (TOTK_PACING=1, opt-in: it misjudges caps that do not divide the refresh rate), or fill mode */
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_PACING"); on = e && atoi(e) > 0; }
    if (!(pace_ext && pace_get_refresh && pace_get_past)) return 0;
    if (on || fg_fill) return 1;
    /* Frame generation whose pictures a second divide the measured refresh rate (60 on 120 Hz): unpaced, a generated
       picture and the real one after it were handed over back to back, and on 120 Hz the generated one stayed a refresh
       and the real one three (Odin, every method: 300 / 272 of 600 pictures); paced, 584 of 600 stayed two. */
    int shown = fg_shown_hz();
    if (shown <= 0 || !pace_refresh || !(fg_enabled() || lsfg_enabled())) return 0;
    double per = 1e9 / (double)pace_refresh / shown;
    return per >= 0.95 && fabs(per - (double)(int)(per + 0.5)) < 0.05;
}
/* The panel's rate can change under us: at critical thermal status Android votes the display down to 60 Hz (DisplayModeDirector's
   skin temperature observer), and no app can overrule it. Frame generation must follow: two pictures a game frame on a
   60 Hz panel take 33 ms, so x2 at 60 would turn a 17 ms game into 30 fps with every picture on screen for "2 refreshes"
   of a 120 Hz panel that is not there. Read every quarter of a second. */
static uint64_t pace_poll_at;
static void pace_refresh_poll(void) {
    uint64_t now = dm_android_clock_ns(CLOCK_MONOTONIC); if (now < pace_poll_at) return; pace_poll_at = now + 250000000;
    VkRefreshCycleDurationGOOGLE r; if (pace_get_refresh(device, swapchain, &r) || !r.refreshDuration) return;
    uint64_t was = pace_refresh, d = r.refreshDuration > was ? r.refreshDuration - was : was - r.refreshDuration;
    /* the measured period wanders a little: only a new mode counts */
    if (was && d * 10 < was) return;
    pace_refresh = r.refreshDuration; pace_anchor = 0;
    if (was) LOG("[pacing] the panel changed: refresh %.2f -> %.2f ms (%.0f Hz)", was / 1e6, pace_refresh / 1e6, 1e9 / pace_refresh);
}
/* The panel's rate for the HUD, 0 unknown: the nearest usual rate, as the measured period wanders (8.22 ms on a 120 Hz panel) */
static uint32_t pace_screen_hz(void) {
    static const uint32_t rates[] = {30, 48, 50, 60, 72, 75, 90, 96, 100, 120, 144, 165, 240};
    if (!pace_refresh) return 0;
    double hz = 1e9 / (double)pace_refresh; uint32_t best = (uint32_t)(hz + 0.5);
    for (size_t i = 0; i < sizeof rates / sizeof *rates; i++) if (fabs(hz - rates[i]) < rates[i] * 0.04) best = rates[i];
    return best;
}
/* Pictures of a game frame the panel can show at the frame cap: 2 at 60 on 120 Hz, 1 at 60 on 60 Hz. */
static int pace_pictures_fit(int cap_hz) {
    if (!pace_refresh || cap_hz <= 0) return 16;
    int n = (int)(1e9 / (double)pace_refresh / cap_hz + 0.1); return n < 1 ? 1 : n;
}
/* after a present: the refresh period and grid, and how long a present takes to show */
static void pace_learn(void) {
    /* the histogram and TL_SHOWN also without pacing */
    if (!pace_ext || !pace_get_refresh || !pace_get_past || !swapchain) return;
    /* each present's GPU time, once its fence has passed */
    if (pace_id - pace_ready_id > 48) pace_ready_id = pace_id - 1;
    while (pace_ready_id < pace_id) {
        uint32_t id = pace_ready_id + 1; uint64_t ticket = pace_ticket[id % 64], seen = atomic_load(&pace_fence_ticket[ticket % 1024]);
        if (seen < ticket) break;
        pace_done[id % 64] = 0;
        if (seen == ticket) { uint64_t done = atomic_load(&pace_fence_ns[ticket % 1024]), submit = pace_submit[id % 64]; pace_done[id % 64] = done;
            if (totk_tl_at) totk_tl_at(TL_READY, done, id, atomic_load(&pace_fence_ticket[(ticket - 1) % 1024]) == ticket - 1 ? atomic_load(&pace_fence_ns[(ticket - 1) % 1024]) : 0);
            uint64_t g = done > submit ? done - submit : 0; if (g > 100000000) g = 100000000;
            /* quick to rise, slow to fall: the slow frames are the ones that drop pictures */
            pace_gpu = g > pace_gpu ? (pace_gpu + g) / 2 : pace_gpu - (pace_gpu - g) / 32; }
        pace_ready_id = id;
    }
    if (pace_chain == swapchain) pace_refresh_poll();
    if (pace_chain != swapchain) { pace_chain = swapchain; pace_anchor = 0; pace_seen_id = 0; VkRefreshCycleDurationGOOGLE r;
        if (!pace_get_refresh(device, swapchain, &r)) { pace_refresh = r.refreshDuration; LOG("[pacing] refresh %.2f ms", r.refreshDuration / 1e6); } }
    VkPastPresentationTimingGOOGLE past[16]; uint32_t n = 16;
    VkResult pr = pace_get_past(device, swapchain, &n, past);
    { static uint64_t queries, entries, with_actual; queries++; if (pr >= 0) { entries += n; for (uint32_t i = 0; i < n; i++) with_actual += past[i].actualPresentTime != 0; }
      if (queries % 600 == 0) LOG("[pacing] past presentation timings: %llu entries (%llu with an actual time) over %llu queries, last result %d", (unsigned long long)entries, (unsigned long long)with_actual, (unsigned long long)queries, (int)pr); }
    if (pr < 0) return;
    /* how long each picture stayed on screen, in refreshes (1..4, 5+; 0 unused) */
    static uint64_t last_actual, hist[6], hist_n;
    for (uint32_t i = 0; i < n; i++) {
        if (!past[i].actualPresentTime) continue;
        /* latency (TOTK_TIMELINE) */
        if (totk_tl_at) totk_tl_at(TL_SHOWN, past[i].actualPresentTime, past[i].presentID, pace_submit[past[i].presentID % 64]);
        if (last_actual && pace_refresh && past[i].actualPresentTime > last_actual) {
            uint64_t k = (past[i].actualPresentTime - last_actual + pace_refresh / 2) / pace_refresh; hist[k < 5 ? k : 5]++;
            if (!(++hist_n % 600)) { LOG("[pacing] on screen for 1/2/3/4/5+ refreshes: %llu %llu %llu %llu %llu (last 600 pictures)",
                (unsigned long long)hist[1], (unsigned long long)hist[2], (unsigned long long)hist[3], (unsigned long long)hist[4], (unsigned long long)hist[5]); memset(hist, 0, sizeof hist); } }
        last_actual = past[i].actualPresentTime;
        pace_anchor = past[i].actualPresentTime;
        if (!pace_on() || !pace_refresh) { pace_seen_id = past[i].presentID; continue; }
        /* Android reports only the pictures that reached the screen: a missing present id was dropped */
        if (pace_seen_id && past[i].presentID > pace_seen_id + 1) pace_missed += past[i].presentID - pace_seen_id - 1;
        if (past[i].presentID > pace_seen_id) pace_seen_id = past[i].presentID;
        /* the compositor's part, from pictures shown later than planned (held back by nothing but their own readiness) */
        uint64_t done = pace_done[past[i].presentID % 64], actual = past[i].actualPresentTime, desired = past[i].desiredPresentTime;
        if (!done || actual <= done || (desired && actual <= desired + pace_refresh)) continue;
        uint64_t c = actual - done; if (c > 4 * pace_refresh) continue;
        pace_comp = pace_comp ? (pace_comp * 7 + c) / 8 : c;
    }
}
/* the first refresh at or after t */
static uint64_t pace_grid_after(uint64_t t) {
    if (!pace_anchor || !pace_refresh) return t;
    if (t <= pace_anchor) return pace_anchor;
    return pace_anchor + (t - pace_anchor + pace_refresh - 1) / pace_refresh * pace_refresh;
}
static uint64_t pace_grid_round(uint64_t t) {
    if (!pace_anchor || !pace_refresh) return t;
    if (t <= pace_anchor) return pace_anchor;
    return pace_anchor + (t - pace_anchor + pace_refresh / 2) / pace_refresh * pace_refresh;
}
/* Before a frame's presents: the times for its generated pictures and then the real one (desired[0..generated]). */
/* game_ms is the game's intended frame time (the frame cap), never the measured one: a measured time includes the waits
   this very pacing adds, and fed back it lengthens every frame until the game crawls at ~3 fps. For the same reason a
   picture is never planned more than one step after the earliest refresh it could be shown at. */
static void pace_plan(int generated, double game_ms, int64_t *desired) {
    for (int i = 0; i <= generated; i++) desired[i] = -1;
    if (!pace_on() || !pace_refresh || !pace_anchor) return;
    /* the compositor's estimate is from refreshes: half a refresh of it is the rounding up to one */
    uint64_t comp = pace_comp > pace_refresh / 2 ? pace_comp - pace_refresh / 2 : 0, lead = pace_gpu + (pace_comp ? comp : pace_refresh / 2);
    uint64_t now = dm_android_clock_ns(CLOCK_MONOTONIC), soonest = pace_grid_after(now + lead), earliest = soonest;
    /* the refresh after the last picture's, rounded to the nearest: the grid is re-learnt from measured times that wander by
       a fraction of a millisecond, and rounding up would now and then skip a refresh, which only ever moves the plan later:
       at a steady frame rate, seconds ahead, past the times the compositor honours */
    uint64_t after_last = pace_last_target ? pace_grid_round(pace_last_target + pace_refresh) : 0;
    if (after_last > earliest) earliest = after_last;
    if (!generated && !fg_fill) { desired[0] = 0; pace_last_target = earliest; return; }
    uint64_t step = (uint64_t)(game_ms * 1e6 / (generated + 1)); if (step < pace_refresh) step = pace_refresh;
    uint64_t t = pace_last_target ? pace_grid_round(pace_last_target + step) : earliest;
    pace_frames++; if (t < earliest) { t = earliest; pace_late++; }
    /* never more than a step after the soonest refresh a picture presented now can reach: a plan that got ahead comes back,
       at the cost of at most one dropped picture, instead of keeping the queue full */
    if (t > pace_grid_round(soonest + step)) t = pace_grid_round(soonest + step);
    for (int i = 0; i <= generated; i++) {
        if (i) { uint64_t next = pace_grid_round(t + step); t = next < t + pace_refresh ? t + pace_refresh : next; }
        /* half a refresh early: that refresh */
        desired[i] = (int64_t)(t - pace_refresh / 2);
    }
    pace_last_target = t;
    if (!(pace_frames % 600)) LOG("[pacing] %llu generated frames, %llu late (shown as soon as possible); %llu pictures not shown; GPU %.1f ms, compositor %.1f ms, refresh %.2f ms",
        (unsigned long long)pace_frames, (unsigned long long)pace_late, (unsigned long long)pace_missed, pace_gpu / 1e6, pace_comp / 1e6, pace_refresh / 1e6);
}

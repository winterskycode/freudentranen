/* Present pacing with frame generation (VK_GOOGLE_display_timing). The generated picture and the real one are presented
   back to back; in FIFO each takes one refresh, so at 120 Hz with the game at 30 the generated picture was on screen for
   8.3 ms and the real one for 25 ms: halfway positions shown for a quarter of the time, a visible stutter on fast pans
   (the glowing door in the Room of Awakening). The display rate is only requested (TOTK_DISPLAY_HZ); other layers can
   keep the panel at 120 Hz. Here each picture gets a "not before" time: with n generated pictures (1, or up to 3 with
   LSFG) each comes 1 / (n + 1) of a game frame after the one before, and the real one the same after the last, on the
   refresh grid learnt from past present times. A late game frame is shown as soon as possible instead. TOTK_PACING=1 turns it on (off by default for now). */
static int pace_ext;   /* VK_GOOGLE_display_timing listed (enabled at device creation) */
void totk_tl_at(int kind, uint64_t t_ns, uint64_t a, uint64_t b) __attribute__((weak));   /* totk_timeline.c */
#include "totk_timeline.h"   /* TL_SHOWN: a picture reached the screen */
static PFN_vkGetRefreshCycleDurationGOOGLE pace_get_refresh;
static PFN_vkGetPastPresentationTimingGOOGLE pace_get_past;
static VkSwapchainKHR pace_chain;
static uint64_t pace_refresh, pace_anchor, pace_last_target, pace_latency = 12000000;   /* ns; anchor: a past refresh */
static uint32_t pace_id;
static int64_t pace_desired = -1;   /* the next present's "not before" time (0: as soon as possible, -1: no timing) */
static uint64_t pace_submit[64];     /* by present id % 64: submit time, for the latency estimate */
static uint64_t pace_frames, pace_late;

static int pace_on(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_PACING"); on = e && atoi(e) > 0; }   /* opt-in (TOTK_PACING=1): it misjudged caps that do not divide the refresh rate */
    return on && pace_ext && pace_get_refresh && pace_get_past;
}
/* The panel's rate can change under us: at critical thermal status Android votes the display down to 60 Hz (DisplayModeDirector's
   skin temperature observer; 2026-11-03: every thermal 4 window on the route), and no app can overrule it. Frame generation
   must follow: two pictures a game frame on a 60 Hz panel take 33 ms, so x2 at 60 turned a 17 ms game into 30 fps while
   every picture sat on screen for "2 refreshes" of the 120 Hz we believed in. Read every quarter of a second. */
static uint64_t pace_poll_at;
static void pace_refresh_poll(void) {
    uint64_t now = dm_android_clock_ns(CLOCK_MONOTONIC); if (now < pace_poll_at) return; pace_poll_at = now + 250000000;
    VkRefreshCycleDurationGOOGLE r; if (pace_get_refresh(device, swapchain, &r) || !r.refreshDuration) return;
    uint64_t was = pace_refresh, d = r.refreshDuration > was ? r.refreshDuration - was : was - r.refreshDuration;
    if (was && d * 10 < was) return;   /* the measured period wanders a little: only a new mode counts */
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
static void pace_learn(void) {   /* after a present: the refresh period and grid, and how long a present takes to show */
    if (!pace_ext || !pace_get_refresh || !pace_get_past || !swapchain) return;   /* the histogram and TL_SHOWN also without pacing */
    if (pace_chain == swapchain) pace_refresh_poll();
    if (pace_chain != swapchain) { pace_chain = swapchain; pace_anchor = 0; VkRefreshCycleDurationGOOGLE r;
        if (!pace_get_refresh(device, swapchain, &r)) { pace_refresh = r.refreshDuration; LOG("[pacing] refresh %.2f ms", r.refreshDuration / 1e6); } }
    VkPastPresentationTimingGOOGLE past[16]; uint32_t n = 16;
    VkResult pr = pace_get_past(device, swapchain, &n, past);
    { static uint64_t queries, entries, with_actual; queries++; if (pr >= 0) { entries += n; for (uint32_t i = 0; i < n; i++) with_actual += past[i].actualPresentTime != 0; }
      if (queries % 600 == 0) LOG("[pacing] past presentation timings: %llu entries (%llu with an actual time) over %llu queries, last result %d", (unsigned long long)entries, (unsigned long long)with_actual, (unsigned long long)queries, (int)pr); }
    if (pr < 0) return;
    static uint64_t last_actual, hist[6], hist_n;   /* how long each picture stayed on screen, in refreshes (1..4, 5+; 0 unused) */
    for (uint32_t i = 0; i < n; i++) {
        if (!past[i].actualPresentTime) continue;
        if (totk_tl_at) totk_tl_at(TL_SHOWN, past[i].actualPresentTime, past[i].presentID, pace_submit[past[i].presentID % 64]);   /* latency (TOTK_TIMELINE) */
        if (last_actual && pace_refresh && past[i].actualPresentTime > last_actual) {
            uint64_t k = (past[i].actualPresentTime - last_actual + pace_refresh / 2) / pace_refresh; hist[k < 5 ? k : 5]++;
            if (!(++hist_n % 600)) { LOG("[pacing] on screen for 1/2/3/4/5+ refreshes: %llu %llu %llu %llu %llu (last 600 pictures)",
                (unsigned long long)hist[1], (unsigned long long)hist[2], (unsigned long long)hist[3], (unsigned long long)hist[4], (unsigned long long)hist[5]); memset(hist, 0, sizeof hist); } }
        last_actual = past[i].actualPresentTime;
        pace_anchor = past[i].actualPresentTime;
        uint64_t submit = pace_submit[past[i].presentID % 64], allowed = submit > past[i].desiredPresentTime ? submit : past[i].desiredPresentTime;
        if (submit && past[i].actualPresentTime > allowed) { uint64_t l = past[i].actualPresentTime - allowed; if (l < 100000000) pace_latency = (pace_latency * 7 + l) / 8; }
    }
}
static uint64_t pace_grid_after(uint64_t t) {   /* the first refresh at or after t */
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
   this very pacing adds, and fed back it lengthened every frame until the game crawled at ~3 fps. For the same reason a
   picture is never planned more than one step after the earliest refresh it could be shown at. */
static void pace_plan(int generated, double game_ms, int64_t *desired) {
    for (int i = 0; i <= generated; i++) desired[i] = -1;
    if (!pace_on() || !pace_refresh || !pace_anchor) return;
    uint64_t now = dm_android_clock_ns(CLOCK_MONOTONIC), earliest = pace_grid_after(now + pace_latency);
    if (pace_last_target && pace_grid_after(pace_last_target + pace_refresh) > earliest) earliest = pace_grid_after(pace_last_target + pace_refresh);
    if (!generated) { desired[0] = 0; pace_last_target = earliest; return; }
    uint64_t step = (uint64_t)(game_ms * 1e6 / (generated + 1)); if (step < pace_refresh) step = pace_refresh;
    uint64_t t = pace_last_target ? pace_grid_round(pace_last_target + step) : earliest;
    pace_frames++; if (t < earliest) { t = earliest; pace_late++; }
    if (t > pace_grid_round(earliest + step)) t = pace_grid_round(earliest + step);   /* no drift into the future */
    for (int i = 0; i <= generated; i++) {
        if (i) { uint64_t next = pace_grid_round(t + step); t = next < t + pace_refresh ? t + pace_refresh : next; }
        desired[i] = (int64_t)(t - pace_refresh / 2);   /* half a refresh early: that refresh */
    }
    pace_last_target = t;
    if (!(pace_frames % 600)) LOG("[pacing] %llu generated frames, %llu late (shown as soon as possible); latency %.1f ms, refresh %.2f ms",
        (unsigned long long)pace_frames, (unsigned long long)pace_late, pace_latency / 1e6, pace_refresh / 1e6);
}

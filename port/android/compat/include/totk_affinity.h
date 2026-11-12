/* CPU affinity of the calling thread from an environment variable (A/B knobs for the thread layout):
   "fast" = the cores with the highest cpuinfo_max_freq (the Odin 3's two prime cores), "fast:N" = the Nth fastest core
   (by cpuinfo_max_freq, the higher-numbered first among equals: the Odin 3's fast:0/fast:1 are its two prime cores,
   cpu7 and cpu6; on an SoC with one prime core, fast:1 is its fastest performance core), "slow" = every core not in "fast",
   "slow:N" = the N highest-numbered cores outside "fast" (every core when there are not more than N of them),
   "all" = every core (undoes a mask inherited from the creating thread), "slow-if-flat" = "slow" on an SoC whose slowest core
   runs at 75% or more of its fastest (the Odin 3's 3.53 / 4.32 GHz: no little cores, so the game's threads lose little
   by leaving the two prime cores to the translation and recording threads: still scene 75 -> 83 fps) and
   "all" elsewhere (a device with little cores keeps its game threads free to use the big ones), or a list of cores and
   ranges like "0-5" or "6,7". Terms joined by "+" are a union: "fast:0+fast:2" is the fastest core and the third fastest.
   Unset: dflt. */
#pragma once
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One term into set (added to it): the cores it names, or -1 for "all" */
static inline int totk_cpus(const char *e, cpu_set_t *out) {
    cpu_set_t set; CPU_ZERO(&set); int n = 0;
    if (!strcmp(e, "all")) { for (int c = 0; c < CPU_SETSIZE; c++) CPU_SET(c, out); return -1; }
    if (!strcmp(e, "slow-if-flat")) {
        long lo = 0, hi = 0;
        for (int c = 0; c < 64; c++) {
            char path[96]; snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
            FILE *f = fopen(path, "r"); if (!f) continue; long v = 0; if (fscanf(f, "%ld", &v) != 1) v = 0; fclose(f);
            if (v) { if (!lo || v < lo) lo = v; if (v > hi) hi = v; }
        }
        return totk_cpus(hi && lo * 4 >= hi * 3 ? "slow" : "all", out);
    }
    if (!strncmp(e, "fast", 4) || !strncmp(e, "slow", 4)) {
        long best = 0, freq[64] = {0};
        for (int c = 0; c < 64; c++) {
            char path[96]; snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
            FILE *f = fopen(path, "r"); if (!f) continue;
            if (fscanf(f, "%ld", &freq[c]) != 1) freq[c] = 0;
            fclose(f); if (freq[c] > best) best = freq[c];
        }
        int fast = e[0] == 'f', pick = e[4] == ':' ? atoi(e + 5) : -1;
        /* "slow:N": the N highest-numbered cores outside the fast set; fewer than N+1 such cores -> every core */
        if (!fast && pick > 0) {
            int slow = 0; for (int c = 0; c < 64; c++) if (best && freq[c] && freq[c] != best) slow++;
            if (slow <= pick) return totk_cpus("all", out);
            for (int c = 63; c >= 0 && n < pick; c--) if (best && freq[c] && freq[c] != best) { CPU_SET(c, &set); n++; }
        /* the pick-th fastest core: rank by max frequency, the higher-numbered first among equals */
        } else if (fast && pick >= 0) {
            int order[64], m = 0;
            for (int c = 63; c >= 0; c--) if (freq[c]) order[m++] = c;
            for (int i = 1; i < m; i++) { int c = order[i], j = i; while (j > 0 && freq[order[j - 1]] < freq[c]) { order[j] = order[j - 1]; j--; } order[j] = c; }
            if (m) { CPU_SET(order[pick < m ? pick : m - 1], &set); n++; }
        } else for (int c = 63; c >= 0; c--) {
            if (!best || !freq[c] || (freq[c] == best) != fast) continue;
            CPU_SET(c, &set); n++;
        }
    } else {
        for (const char *p = e; *p;) {
            char *end; long a = strtol(p, &end, 10), b = a;
            if (end == p) break;
            if (*end == '-') { const char *q = end + 1; b = strtol(q, &end, 10); if (end == q) b = a; }
            for (long c = a; c <= b; c++) if (c >= 0 && c < CPU_SETSIZE) { CPU_SET((int)c, &set); n++; }
            p = *end ? end + 1 : end;
        }
    }
    CPU_OR(out, out, &set);
    return n;
}
/* The calling thread's affinity from env (or dflt): the cores pinned to, 0 when not pinned ("all", nothing valid) */
static inline int totk_pin_self(const char *env, const char *dflt) {
    const char *e = getenv(env);
    if (!e || !*e) e = dflt;
    if (!e || !*e) return 0;
    cpu_set_t set; CPU_ZERO(&set); int all = 0;
    char buf[128]; snprintf(buf, sizeof buf, "%s", e);
    for (char *term = buf, *next; term; term = next) {
        next = strchr(term, '+'); if (next) *next++ = 0;
        if (*term && totk_cpus(term, &set) < 0) all = 1;
    }
    int n = CPU_COUNT(&set);
    if (!n) return 0;
    sched_setaffinity(0, sizeof set, &set);
    return all ? 0 : n;
}

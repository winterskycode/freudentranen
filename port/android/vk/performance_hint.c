#include "performance_hint.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <android/performance_hint.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Resolved at run time, so the library still loads below API 33 (setThreads is API 34). */
typedef struct {
    APerformanceHintManager *(*manager)(void);
    APerformanceHintSession *(*create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
    int (*report)(APerformanceHintSession *, int64_t);
    int (*update_target)(APerformanceHintSession *, int64_t);
    int (*set_threads)(APerformanceHintSession *, const int32_t *, size_t);
    int64_t (*preferred_rate)(APerformanceHintManager *);
} hint_api;

enum { MAX_TIDS = 128 };
static hint_api api;
static pthread_once_t load_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t hint_lock = PTHREAD_MUTEX_INITIALIZER;
static APerformanceHintSession *session;
static int available, failed, dirty;
static int32_t tids[MAX_TIDS]; static size_t ntids;
static int64_t target_ns;
static uint64_t last_frame, reports, report_errors, work_sum;

static uint64_t monotonic_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

static void load_api(void) {
    const char *e = getenv("TOTK_ADPF");
    if (e && !strcmp(e, "0")) { fprintf(stderr, "[adpf] off (TOTK_ADPF=0)\n"); return; }
    /* kept open for the process's life */
    void *library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "[adpf] libandroid not loadable\n"); return; }
#define LOAD_HINT(field, name) do { void *symbol = dlsym(library, name); memcpy(&api.field, &symbol, sizeof api.field); } while (0)
    LOAD_HINT(manager, "APerformanceHint_getManager");
    LOAD_HINT(create, "APerformanceHint_createSession");
    LOAD_HINT(report, "APerformanceHint_reportActualWorkDuration");
    LOAD_HINT(update_target, "APerformanceHint_updateTargetWorkDuration");
    LOAD_HINT(set_threads, "APerformanceHint_setThreads");
    LOAD_HINT(preferred_rate, "APerformanceHint_getPreferredUpdateRateNanos");
#undef LOAD_HINT
    available = api.manager && api.create && api.report && api.update_target;
    fprintf(stderr, "[adpf] API %s (setThreads %s)\n", available ? "available" : "missing", api.set_threads ? "yes" : "no");
}

void dm_android_perf_hint_thread(void) {
    pthread_once(&load_once, load_api);
    if (!available) return;
    int32_t tid = gettid();
    pthread_mutex_lock(&hint_lock);
    if (ntids < MAX_TIDS) { tids[ntids++] = tid; dirty = 1; }
    pthread_mutex_unlock(&hint_lock);
}

/* hle_nvn.c: the uncapped clock's ceiling, 60 when paced */
extern int nvn_frame_cap_get(void) __attribute__((weak));
static int64_t wanted_target(void) {
    int cap = nvn_frame_cap_get ? nvn_frame_cap_get() : 60;
    return (int64_t)(1e9 / (cap > 0 ? cap : 60));
}

/* Holding hint_lock: drop threads that have exited (the game's short-lived threads); setThreads refuses a list with one. */
static void prune_locked(void) {
    size_t k = 0;
    for (size_t i = 0; i < ntids; i++) { char path[48]; snprintf(path, sizeof path, "/proc/self/task/%d", tids[i]); if (!access(path, F_OK)) tids[k++] = tids[i]; }
    ntids = k;
}
/* Holding hint_lock: create the session, or hand it the current thread list. */
static void session_update_locked(void) {
    if (!dirty || failed) return;
    dirty = 0; prune_locked();
    if (!session) {
        APerformanceHintManager *m = api.manager();
        target_ns = wanted_target();
        session = m ? api.create(m, tids, ntids, target_ns) : NULL;
        if (!session) { failed = 1; fprintf(stderr, "[adpf] session not created (%zu threads); normal scheduling\n", ntids); return; }
        fprintf(stderr, "[adpf] session: %zu threads, target %.2f ms, preferred report interval %.2f ms\n", ntids, target_ns / 1e6,
                api.preferred_rate ? api.preferred_rate(m) / 1e6 : -1.0);
    } else if (api.set_threads) {
        int r = api.set_threads(session, tids, ntids);
        if (r) fprintf(stderr, "[adpf] setThreads(%zu) failed: %d\n", ntids, r);
    }
}

void dm_android_perf_hint_frame(void) {
    pthread_once(&load_once, load_api);
    if (!available) return;
    uint64_t now = monotonic_ns();
    pthread_mutex_lock(&hint_lock);
    session_update_locked();
    uint64_t work = last_frame ? now - last_frame : 0; last_frame = now;
    if (session && work && work < 1000000000ull) {
        int64_t want = wanted_target();
        /* the cap changed */
        if (want != target_ns && !api.update_target(session, want)) target_ns = want;
        if (api.report(session, (int64_t)work)) report_errors++; else { reports++; work_sum += work; }
        if (reports && reports % 1200 == 0)
            fprintf(stderr, "[adpf] %llu reports (%llu errors), mean frame %.2f ms against target %.2f ms, %zu threads\n",
                    (unsigned long long)reports, (unsigned long long)report_errors, work_sum / 1e6 / (double)reports, target_ns / 1e6, ntids);
    }
    pthread_mutex_unlock(&hint_lock);
}

#else
void dm_android_perf_hint_thread(void) {}
void dm_android_perf_hint_frame(void) {}
#endif

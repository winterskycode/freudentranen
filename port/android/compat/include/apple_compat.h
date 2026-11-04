/* Force-included (-include) into every Android translation unit: the Darwin libc extras MacNativeGames uses. */
#pragma once
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static inline int compat_pthread_threadid_np(pthread_t thread, uint64_t *id) {
    *id = (uint64_t)(thread ? pthread_gettid_np(thread) : gettid()); return 0;
}
#define pthread_threadid_np(thread, id) compat_pthread_threadid_np((pthread_t)(thread), (id))
/* Darwin's pthread_setname_np(name) names the calling thread; Linux takes (thread, name) and at most 15 characters. */
#include <string.h>
static inline int compat_setname(pthread_t t, const char *name) { char n[16]; strncpy(n, name, 15); n[15] = 0; return (pthread_setname_np)(t, n); }
#define COMPAT_SETNAME_PICK(_1, _2, NAME, ...) NAME
#define COMPAT_SETNAME1(name) compat_setname(pthread_self(), (name))
#define COMPAT_SETNAME2(t, name) compat_setname((t), (name))
#define pthread_setname_np(...) COMPAT_SETNAME_PICK(__VA_ARGS__, COMPAT_SETNAME2, COMPAT_SETNAME1)(__VA_ARGS__)
/* Darwin's stack queries: the address is the high end (stack top). */
static inline void *pthread_get_stackaddr_np(pthread_t t) {
    pthread_attr_t a; void *lo = 0; size_t n = 0;
    if (!pthread_getattr_np(t, &a)) { pthread_attr_getstack(&a, &lo, &n); pthread_attr_destroy(&a); }
    return (char *)lo + n;
}
static inline size_t pthread_get_stacksize_np(pthread_t t) {
    pthread_attr_t a; void *lo = 0; size_t n = 0;
    if (!pthread_getattr_np(t, &a)) { pthread_attr_getstack(&a, &lo, &n); pthread_attr_destroy(&a); }
    return n;
}
/* Sleep until an absolute mach_absolute_time() deadline (nanoseconds on CLOCK_MONOTONIC here). */
static inline int mach_wait_until(uint64_t deadline) {
    struct timespec t = {(time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, 0) == 4 /* EINTR */) {}
    return 0;
}
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#define CLOCK_MONOTONIC_RAW_APPROX CLOCK_MONOTONIC_COARSE
#define CLOCK_UPTIME_RAW_APPROX CLOCK_MONOTONIC_COARSE
static inline uint64_t clock_gettime_nsec_np(clockid_t clock) {
    struct timespec t; clock_gettime(clock, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
/* Darwin stat time fields; Linux has no birth time in struct stat, the change time stands in for it. */
#define st_mtimespec st_mtim
#define st_ctimespec st_ctim
#define st_atimespec st_atim
#define st_birthtimespec st_ctim
/* pthread_cond_timedwait_relative_np: wait at most rel (CLOCK_REALTIME, as condition variables default to). */
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += rel->tv_sec; t.tv_nsec += rel->tv_nsec;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, m, &t);
}

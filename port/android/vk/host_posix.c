// Copy save directories and atomically exchange with renameat2, preserving old commits, timestamps, and user xattrs.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "host_posix.h"
#include <dirent.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <sys/uio.h>
#include <unistd.h>

uint64_t dm_android_clock_ns(clockid_t clock) {
    struct timespec t;
    if (clock_gettime(clock, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static _Thread_local dm_android_read_stats current_thread_read_stats;
/* Fault-safe reads of guest memory: a plain memcpy under a thread-local recovery point. A SIGSEGV/SIGBUS while the copy
   runs jumps back and the read is retried through process_vm_readv, which reports the partial or failed read. Using
   process_vm_readv alone, a syscall per read, takes 44% of the render thread in game. Other faults go to the handler
   installed before ours. */
static _Thread_local sigjmp_buf *safe_read_jump;
static struct sigaction safe_read_previous[2];
static void safe_read_fault(int sig, siginfo_t *info, void *context) {
    sigjmp_buf *jump = safe_read_jump;
    if (jump) { safe_read_jump = NULL; siglongjmp(*jump, 1); }
    struct sigaction *previous = &safe_read_previous[sig == SIGBUS];
    if (previous->sa_flags & SA_SIGINFO) { previous->sa_sigaction(sig, info, context); return; }
    if (previous->sa_handler == SIG_IGN) return;
    if (previous->sa_handler == SIG_DFL) { signal(sig, SIG_DFL); raise(sig); return; }
    previous->sa_handler(sig);
}
static void safe_read_install(void) {
    struct sigaction action = {0};
    action.sa_sigaction = safe_read_fault; action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK; sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &safe_read_previous[0]); sigaction(SIGBUS, &action, &safe_read_previous[1]);
}
static int read_memory_syscall(uintptr_t address, void *destination, size_t size);
/* one installation: a second sigaction would record our own handler as the previous one */
static pthread_once_t safe_read_once = PTHREAD_ONCE_INIT;
int dm_android_read_memory(uintptr_t address, void *destination, size_t size) {
    pthread_once(&safe_read_once, safe_read_install);
    sigjmp_buf jump;
    /* SA_NODEFER: no mask to restore */
    if (sigsetjmp(jump, 0)) return read_memory_syscall(address, destination, size);
    safe_read_jump = &jump;
    memcpy(destination, (const void *)address, size);
    safe_read_jump = NULL;
    dm_android_read_stats *stats = &current_thread_read_stats;
    unsigned bucket = !size ? 0 : size <= 64 ? 1 : size <= 256 ? 2 : size <= 1024 ? 3 : size <= 4096 ? 4 : size <= 65536 ? 5 : 6;
    stats->calls++; stats->requested_bytes += size; stats->returned_bytes += size; stats->full++;
    stats->size_buckets[bucket]++; stats->bucket_requested_bytes[bucket] += size; stats->bucket_returned_bytes[bucket] += size;
    return 0;
}
/* n ranges ({lo, hi} pairs) copied to base + off[i] under one recovery point; a range that faults is retried alone
   through dm_android_read_memory and zeroed if that fails too. Returns the number of ranges zeroed. The submit
   capture copies about 1,300 ranges a frame on the game thread: one setjmp and one once-check instead of 1,300. */
int dm_android_read_ranges(int n, const uint64_t *ranges, uint8_t *base, const size_t *off) {
    pthread_once(&safe_read_once, safe_read_install);
    sigjmp_buf jump; volatile int i = 0; int zeroed = 0; size_t bytes = 0;
    /* range i faulted: the slow, reporting way for it, then on with the next */
    if (sigsetjmp(jump, 0)) {
        size_t size = (size_t)(ranges[2 * i + 1] - ranges[2 * i]);
        if (dm_android_read_memory(ranges[2 * i], base + off[i], size)) { memset(base + off[i], 0, size); zeroed++; }
        i = i + 1; }
    for (; i < n; i = i + 1) { size_t size = (size_t)(ranges[2 * i + 1] - ranges[2 * i]); safe_read_jump = &jump; memcpy(base + off[i], (const void *)(uintptr_t)ranges[2 * i], size); bytes += size; }
    safe_read_jump = NULL;
    dm_android_read_stats *stats = &current_thread_read_stats; stats->calls += (unsigned)n; stats->requested_bytes += bytes; stats->returned_bytes += bytes; stats->full += (unsigned)n;
    return zeroed;
}
static int read_memory_syscall(uintptr_t address, void *destination, size_t size) {
    struct iovec local = {destination, size}, remote = {(void *)address, size};
    // Take two actual wall readings; preserve errno before/after the syscall without changing the safe-read result.
    struct timespec before, after; int incoming_errno = errno;
    int before_ok = !clock_gettime(CLOCK_MONOTONIC, &before);
    errno = incoming_errno;
    ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    int read_errno = errno;
    int after_ok = !clock_gettime(CLOCK_MONOTONIC, &after);
    dm_android_read_stats *stats = &current_thread_read_stats;
    stats->calls++; stats->requested_bytes += size;
    if (n >= 0) {
        stats->returned_bytes += (uint64_t)n;
        if ((size_t)n == size) stats->full++; else stats->partial++;
    } else stats->errors++;
    unsigned bucket = !size ? 0 : size <= 64 ? 1 : size <= 256 ? 2 : size <= 1024 ? 3 : size <= 4096 ? 4 : size <= 65536 ? 5 : 6;
    stats->size_buckets[bucket]++;
    stats->bucket_requested_bytes[bucket] += size;
    if (n >= 0) stats->bucket_returned_bytes[bucket] += (uint64_t)n;
    if (before_ok && after_ok) {
        uint64_t start = (uint64_t)before.tv_sec * 1000000000ull + before.tv_nsec;
        uint64_t finish = (uint64_t)after.tv_sec * 1000000000ull + after.tv_nsec;
        if (finish >= start) {
            uint64_t elapsed = finish - start;
            stats->wall_ns += elapsed; stats->bucket_wall_ns[bucket] += elapsed;
        } else { stats->wall_unavailable++; stats->bucket_wall_unavailable[bucket]++; }
    } else { stats->wall_unavailable++; stats->bucket_wall_unavailable[bucket]++; }
    errno = read_errno;
    return n >= 0 && (size_t)n == size ? 0 : -1;
}


// thread_selfcounts is Apple's private libSystem counter API, used by the host's dm_prof.c (DM_PUMP_PROF=1). host_link.py
// links with -z defs, so it must be defined here; Android has no counters, so it reads 0 and returns -1, like the Android
// copy in hle_nvn.c.
__attribute__((weak)) int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; memset(buf, 0, nbytes); return -1; }

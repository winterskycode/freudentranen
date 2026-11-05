// Android host time, thread stacks, and save transactions.
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
uint64_t dm_android_clock_ns(clockid_t clock);
int dm_android_read_memory(uintptr_t address, void *destination, size_t size);
enum { DM_ANDROID_READ_SIZE_BUCKETS = 7 };
typedef struct dm_android_read_stats {
    uint64_t calls, requested_bytes, returned_bytes, full, partial, errors;
    uint64_t size_buckets[DM_ANDROID_READ_SIZE_BUCKETS], wall_ns, wall_unavailable;
    uint64_t bucket_requested_bytes[DM_ANDROID_READ_SIZE_BUCKETS];
    uint64_t bucket_returned_bytes[DM_ANDROID_READ_SIZE_BUCKETS];
    uint64_t bucket_wall_ns[DM_ANDROID_READ_SIZE_BUCKETS];
    uint64_t bucket_wall_unavailable[DM_ANDROID_READ_SIZE_BUCKETS];
    uint32_t tid;
} dm_android_read_stats;
// Cumulative snapshot of this host thread; never reset or combine other threads; full includes successful zero-byte reads.
// Cover every use of this reader on this thread, including non-NVN calls; not a process-wide/all-thread total.
// Bucket order: 0, 1..64, 65..256, 257..1024, 1025..4096, 4097..65536, >65536 bytes.
// Per-bucket bytes/wall use the same actual read and clock pair; their sums equal the totals.
// Wall is an actual monotonic-clock delta, including time scheduled out during the call, not kernel CPU; clock failures/regressions count as unavailable.

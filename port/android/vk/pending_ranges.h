// Exact per-batch address interval index; the caller reuses it only after the real fence completes.
#ifndef DM_PENDING_RANGES_H
#define DM_PENDING_RANGES_H
#include <stddef.h>
#include <stdint.h>

typedef enum { DM_PENDING_UNKNOWN, DM_PENDING_HOST_EFFECT, DM_PENDING_KIND_COUNT } dm_pending_kind;
#define DM_PENDING_ORDER_ALL UINT64_MAX
typedef struct dm_pending_range_node dm_pending_range_node;
typedef struct dm_pending_range_chunk dm_pending_range_chunk;
typedef struct dm_pending_range_version dm_pending_range_version;
typedef struct {
    dm_pending_range_version *versions;
    size_t count,capacity;
} dm_pending_range_collection;
typedef struct {
    dm_pending_range_collection collections[DM_PENDING_KIND_COUNT];
    dm_pending_range_chunk *chunks,*current;
    size_t live_nodes,retained_bytes;
} dm_pending_ranges;

// Initialize with all zeros; valid nonempty registration returns 1, zero length 0, errors -1 with errno.
// Orders strictly increase per kind and are below ALL; known CPU copies never erase unknown records.
int dm_pending_ranges_insert(dm_pending_ranges *ranges,dm_pending_kind kind,uintptr_t address,uint64_t size,uint64_t order);

// Query the actual half-open interval, including only order < before_order; ALL selects the whole batch, 0 an empty prefix.
// Return 1 for overlap, 0 otherwise, -1 for invalid arguments; optional visited counts actual tree nodes visited.
int dm_pending_ranges_overlap(const dm_pending_ranges *ranges,dm_pending_kind kind,uintptr_t address,uint64_t size,uint64_t before_order,size_t *visited);

// Reset retains peak capacity; destroy releases memory. Neither may run while its batch is still in flight.
void dm_pending_ranges_reset(dm_pending_ranges *ranges);
#endif

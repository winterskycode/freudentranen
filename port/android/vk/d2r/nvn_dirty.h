// Write-protect dirty-page tracking for guest memory (nvn_dirty.c)
#pragma once
#include <stddef.h>
#include <stdint.h>

int nvn_dirty_tracked(const void *addr, size_t len);
// Set the covered pages read-only
void nvn_dirty_protect(const void *addr, size_t len);
// Current generation: taken before upload, as the stamp of that content
uint64_t nvn_dirty_now(void);
// Covered pages were written after the stamp -> 1
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp);
// write guest memory on behalf of the GPU: not counted as a CPU write (NULL: not tracked)
void *nvn_dirty_gpu_write_begin(void *addr, size_t len);
void nvn_dirty_gpu_write_end(void *tok);

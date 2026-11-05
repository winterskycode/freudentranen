/* Where shader code lives. A translated shader is usually 70-200 KB, past what Android's allocator keeps in its size
   classes, so each malloc of one is its own mapping with guard pages around it: about two of the process's 65,530
   (vm.max_map_count). Every shader module holds a copy of its SPIR-V in the driver, and every vertex shader one more for
   stage linking, so a long session reaches the limit on these alone (5,450 of the heap's 5,513 large blocks; mprotect
   and vkMapMemory then fail). Here they share large chunks instead: one mapping for many shaders.

   spv_callbacks goes to every vkCreateShaderModule and vkDestroyShaderModule of a game shader, and Vulkan requires the two
   to match: a module made with it must be destroyed with it. A chunk is unmapped once nothing in it is alive. A block may be plain malloc memory when no chunk can be had. */
#include <sys/mman.h>
#include <sys/prctl.h>
#include <linux/prctl.h>
#define SPV_CHUNK ((size_t)32 << 20)
#define SPV_CHUNKS 512
/* 16 bytes, before each block */
typedef struct { size_t size; uint32_t chunk, magic; } spv_header;
enum { SPV_MAGIC = 0x53505641 };
static struct { uint8_t *base[SPV_CHUNKS]; size_t size[SPV_CHUNKS]; uint32_t live[SPV_CHUNKS]; unsigned current; size_t used; int started; } spv_arena;
static pthread_mutex_t spv_arena_lock = PTHREAD_MUTEX_INITIALIZER;

/* the slot of a fresh chunk of at least need bytes, or -1; the lock is held */
static int spv_new_chunk(size_t need) {
    size_t size = need > SPV_CHUNK ? (need + 0xffff) & ~(size_t)0xffff : SPV_CHUNK;
    for (unsigned i = 0; i < SPV_CHUNKS; i++) if (!spv_arena.base[i]) {
        void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return -1;
        prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, p, size, "totk:spirv");
        spv_arena.base[i] = p; spv_arena.size[i] = size; spv_arena.live[i] = 0;
        return (int)i;
    }
    return -1;
}
static void *spv_alloc(size_t size, size_t align) {
    if (align < 16) align = 16;
    pthread_mutex_lock(&spv_arena_lock);
    size_t need = size + sizeof(spv_header) + align;
    unsigned c = spv_arena.current;
    if (!spv_arena.started || spv_arena.used + need > spv_arena.size[c]) {
        int fresh = spv_new_chunk(need);
        if (fresh < 0) {
            pthread_mutex_unlock(&spv_arena_lock);
            void *p = NULL;
            return posix_memalign(&p, align, size) ? NULL : p;
        }
        if (spv_arena.started && !spv_arena.live[c]) { munmap(spv_arena.base[c], spv_arena.size[c]); spv_arena.base[c] = NULL; }
        c = spv_arena.current = (unsigned)fresh; spv_arena.used = 0; spv_arena.started = 1;
    }
    uintptr_t at = (uintptr_t)spv_arena.base[c] + spv_arena.used + sizeof(spv_header);
    at = (at + align - 1) & ~(uintptr_t)(align - 1);
    spv_header *h = (spv_header *)at - 1; h->size = size; h->chunk = c; h->magic = SPV_MAGIC;
    spv_arena.used = at + size - (uintptr_t)spv_arena.base[c];
    spv_arena.live[c]++;
    pthread_mutex_unlock(&spv_arena_lock);
    return (void *)at;
}
/* the lock is held */
static int spv_owns(const void *p) {
    for (unsigned i = 0; i < SPV_CHUNKS; i++)
        if (spv_arena.base[i] && (const uint8_t *)p >= spv_arena.base[i] && (const uint8_t *)p < spv_arena.base[i] + spv_arena.size[i]) return 1;
    return 0;
}
/* A block of the arena, or plain malloc memory (a shader override's code): either is released. */
static void spv_free(void *p) {
    if (!p) return;
    pthread_mutex_lock(&spv_arena_lock);
    if (!spv_owns(p)) { pthread_mutex_unlock(&spv_arena_lock); free(p); return; }
    spv_header *h = (spv_header *)p - 1; unsigned c = h->chunk;
    if (h->magic != SPV_MAGIC) die("Vulkan shader code arena: bad block");
    h->magic = 0;
    if (!--spv_arena.live[c] && c != spv_arena.current) { munmap(spv_arena.base[c], spv_arena.size[c]); spv_arena.base[c] = NULL; }
    else {
        /* Space is not reused, but the pages go back: a shader built only to warm the pipeline library stays out of memory. */
        uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE), lo = ((uintptr_t)p + page - 1) & ~(page - 1), hi = ((uintptr_t)p + h->size) & ~(page - 1);
        if (hi > lo) madvise((void *)lo, hi - lo, MADV_DONTNEED);
    }
    pthread_mutex_unlock(&spv_arena_lock);
}
/* Moves malloc'ed code into the arena; the code stays where it is if the arena has no room. */
static uint32_t *spv_keep(uint32_t *code, size_t bytes) {
    uint32_t *kept = spv_alloc(bytes, 16);
    if (!kept) return code;
    memcpy(kept, code, bytes); free(code);
    return kept;
}
static void *VKAPI_PTR spv_cb_alloc(void *user, size_t size, size_t align, VkSystemAllocationScope scope) { (void)user; (void)scope; return spv_alloc(size, align); }
static void VKAPI_PTR spv_cb_free(void *user, void *p) { (void)user; spv_free(p); }
static void *VKAPI_PTR spv_cb_realloc(void *user, void *old, size_t size, size_t align, VkSystemAllocationScope scope) {
    (void)user; (void)scope;
    if (!old) return spv_alloc(size, align);
    if (!size) { spv_free(old); return NULL; }
    pthread_mutex_lock(&spv_arena_lock); int owned = spv_owns(old); pthread_mutex_unlock(&spv_arena_lock);
    if (!owned) return realloc(old, size);
    void *fresh = spv_alloc(size, align); if (!fresh) return NULL;
    size_t had = ((spv_header *)old - 1)->size;
    memcpy(fresh, old, had < size ? had : size); spv_free(old);
    return fresh;
}
static const VkAllocationCallbacks spv_callbacks = {.pfnAllocation = spv_cb_alloc, .pfnReallocation = spv_cb_realloc, .pfnFree = spv_cb_free};

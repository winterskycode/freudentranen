/* Image memory, suballocated (TOTK_VK_IMAGE_POOL=0: one allocation an image). vkAllocateMemory is a kernel allocation, about
   0.2 ms a MB on the Odin: the frames arriving in a new area spent 16-28 ms of the translation thread on it, the first
   world frame after a load 72 ms (350 MB, 2026-11-03). Images are carved out of 64 MB blocks instead, and a background thread
   keeps TOTK_VK_IMAGE_RESERVE_MB (default 256) of empty blocks ready and gives surplus ones back, so neither the kernel's
   allocation nor its free happens on the translation thread. An image too large for a block, or met with no room left,
   is allocated by itself as before. Recycled memory is not zeroed: a new image's contents are undefined in Vulkan, and
   the free list of whole images has always handed out old contents. */
enum { IMAGE_BLOCK_BYTES = 64u << 20, IMAGE_POOL_LARGEST = 32u << 20 };
typedef struct image_range { VkDeviceSize offset, size; struct image_range *next; } image_range;   /* a block's free ranges, by offset */
typedef struct image_block { VkDeviceMemory memory; unsigned images; image_range *free; struct image_block *next; } image_block;
typedef struct image_memory { VkDeviceMemory memory; VkDeviceSize offset, size; image_block *block; } image_memory;
static image_block *image_blocks;
static pthread_mutex_t image_pool_lock = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t image_pool_cv = PTHREAD_COND_INITIALIZER;
static unsigned image_pool_blocks, image_pool_empty, image_pool_reserve;
static uint32_t image_pool_type = UINT32_MAX; static int image_pool_state = -1;
static uint64_t image_pool_carved, image_pool_direct, image_pool_used;
static void image_pool_report(const char *why) {
    LOG("[vulkan-image-pool] %s: %u blocks (%u MB), %u empty, %.0f MB in use; %llu images carved, %llu allocated alone", why, image_pool_blocks, image_pool_blocks * (IMAGE_BLOCK_BYTES >> 20), image_pool_empty,
        image_pool_used / 1048576.0, (unsigned long long)image_pool_carved, (unsigned long long)image_pool_direct);
}
static void *image_pool_main(void *unused) {
    (void)unused; pthread_setname_np(pthread_self(), "totk-imagepool"); totk_pin_self("TOTK_IMAGE_POOL_CPUS", "slow");
    pthread_mutex_lock(&image_pool_lock);
    for (;;) {
        while (image_pool_empty >= image_pool_reserve && image_pool_empty <= image_pool_reserve + 2) pthread_cond_wait(&image_pool_cv, &image_pool_lock);
        if (image_pool_empty < image_pool_reserve) {
            pthread_mutex_unlock(&image_pool_lock);
            image_block *b = calloc(1, sizeof *b); image_range *r = calloc(1, sizeof *r); if (!b || !r) die("Vulkan image pool allocation");
            VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = IMAGE_BLOCK_BYTES, .memoryTypeIndex = image_pool_type};
            VkResult got = vkAllocateMemory(device, &mi, NULL, &b->memory);
            pthread_mutex_lock(&image_pool_lock);
            if (got != VK_SUCCESS) { free(b); free(r); image_pool_reserve = 0; image_pool_report("out of memory, no reserve from now on"); continue; }
            r->size = IMAGE_BLOCK_BYTES; b->free = r; b->next = image_blocks; image_blocks = b; image_pool_blocks++; image_pool_empty++;
            if (!(image_pool_blocks % 8)) image_pool_report("grown");
        } else {   /* over the reserve: give an empty block back */
            image_block **link = &image_blocks; while (*link && (*link)->images) link = &(*link)->next;
            image_block *b = *link; if (!b) { image_pool_empty = 0; continue; }
            *link = b->next; image_pool_blocks--; image_pool_empty--;
            pthread_mutex_unlock(&image_pool_lock);
            vkFreeMemory(device, b->memory, NULL); while (b->free) { image_range *r = b->free; b->free = r->next; free(r); } free(b);
            pthread_mutex_lock(&image_pool_lock);
        }
    }
    return NULL;
}
/* The first fit in one block; 1 if placed. */
static int image_block_carve(image_block *b, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize *offset) {
    for (image_range **link = &b->free; *link; link = &(*link)->next) {
        image_range *r = *link; VkDeviceSize at = (r->offset + alignment - 1) / alignment * alignment, end = r->offset + r->size;
        if (at + size > end) continue;
        VkDeviceSize tail = end - (at + size);
        if (at > r->offset) {   /* the alignment's gap stays free in front */
            r->size = at - r->offset;
            if (tail) { image_range *t = malloc(sizeof *t); if (!t) die("Vulkan image pool allocation"); *t = (image_range){at + size, tail, r->next}; r->next = t; }
        } else if (tail) { r->offset = at + size; r->size = tail; }
        else { *link = r->next; free(r); }
        *offset = at; return 1;
    }
    return 0;
}
static void image_memory_take(const VkMemoryRequirements *req, uint32_t type, image_memory *out) {
    if (image_pool_state < 0) {
        const char *e = getenv("TOTK_VK_IMAGE_POOL"); image_pool_state = !(e && !atoi(e)); image_pool_type = type;
        e = getenv("TOTK_VK_IMAGE_RESERVE_MB"); int mb = e ? atoi(e) : 256; image_pool_reserve = (unsigned)(mb > 0 ? mb : 0) / (IMAGE_BLOCK_BYTES >> 20);
        if (image_pool_state) { pthread_t t; if (pthread_create(&t, NULL, image_pool_main, NULL)) image_pool_state = 0; else pthread_detach(t); }
        LOG("[vulkan-image-pool] images suballocated: %s", image_pool_state ? "on" : "off");
    }
    *out = (image_memory){.size = req->size};
    if (image_pool_state && type == image_pool_type && req->size <= IMAGE_POOL_LARGEST) {
        pthread_mutex_lock(&image_pool_lock);
        /* blocks in use first: the empty ones are the reserve */
        for (int empty = 0; empty < 2 && !out->block; empty++) for (image_block *b = image_blocks; b; b = b->next) {
            if (!b->images != empty || !image_block_carve(b, req->size, req->alignment, &out->offset)) continue;
            if (!b->images++) { image_pool_empty--; pthread_cond_signal(&image_pool_cv); }
            out->block = b; out->memory = b->memory; image_pool_used += req->size; image_pool_carved++; break;
        }
        if (!out->block) pthread_cond_signal(&image_pool_cv);
        pthread_mutex_unlock(&image_pool_lock);
        if (out->block) return;
    }
    image_pool_direct++;
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req->size, .memoryTypeIndex = type};
    VK_CHECK(vkAllocateMemory(device, &mi, NULL, &out->memory));
}
static void image_memory_give(const image_memory *m) {
    if (!m->block) { vkFreeMemory(device, m->memory, NULL); return; }
    image_block *b = m->block; VkDeviceSize at = m->offset, size = m->size;
    pthread_mutex_lock(&image_pool_lock);
    image_range **link = &b->free, *before = NULL; while (*link && (*link)->offset < at) { before = *link; link = &(*link)->next; }
    image_range *after = *link;
    if (before && before->offset + before->size == at) {   /* joins the range in front, perhaps the one behind too */
        before->size += size;
        if (after && at + size == after->offset) { before->size += after->size; before->next = after->next; free(after); }
    } else if (after && at + size == after->offset) { after->offset = at; after->size += size; }
    else { image_range *r = malloc(sizeof *r); if (!r) die("Vulkan image pool allocation"); *r = (image_range){at, size, after}; *link = r; }
    image_pool_used -= size;
    if (!--b->images) { image_pool_empty++; pthread_cond_signal(&image_pool_cv); }
    pthread_mutex_unlock(&image_pool_lock);
}

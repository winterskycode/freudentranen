/* Freeze-frame (debug): the NVN front end (hle_nvn.c) records one frame's jobs and replays them in a loop while the game
   waits; these helpers make every replay start from the same GPU state. At freeze time every live image is copied into a
   shadow image with the same creation parameters; before each replay the shadows are copied back, so effects that read
   the previous frame (temporal filters, the water simulation, exposure) see the same input every time and the frame comes
   out identical. Images created during the frozen frame are not in the snapshot (they keep the last replay's contents).
   Zero-copy pools (dma-buf memory the GPU writes directly: particle simulations, counters) get shadow buffers the same
   way. Smaller guest memory is not restored. */
typedef struct frz_shadow { image_entry *e; VkImage image; VkDeviceMemory memory; struct frz_shadow *next; } frz_shadow;
static frz_shadow *frz_shadows;
/* Guest ranges the GPU writes back during the recorded frame (post_register_range): saved when the recording ends and
   written back before each replay, so GPU-side state kept in ordinary guest memory (particle simulations on drivers
   without zero-copy) starts each replay the same. Saved after the frame rather than before it: still the same every
   replay, which is what matters. */
typedef struct { uintptr_t lo, hi; uint8_t *bytes; } frz_range;
static frz_range *frz_ranges; static int frz_nranges, frz_capranges;
static void frz_note(uintptr_t address, uint64_t size) {
    if (frz_nranges == frz_capranges) { frz_capranges = frz_capranges ? frz_capranges * 2 : 1024; frz_ranges = realloc(frz_ranges, sizeof *frz_ranges * (size_t)frz_capranges); if (!frz_ranges) die("freeze ranges"); }
    frz_ranges[frz_nranges++] = (frz_range){address, address + size, NULL};
}
static int frz_range_cmp(const void *a, const void *b) { const frz_range *x = a, *y = b; return x->lo < y->lo ? -1 : x->lo > y->lo; }
void mtl_freeze_frozen(void) {   /* the recording ended: keep the GPU-written guest bytes as they are now */
    frz_noting = 0; mtl_finish();
    if (frz_nranges) qsort(frz_ranges, (size_t)frz_nranges, sizeof *frz_ranges, frz_range_cmp);
    int k = -1; for (int i = 0; i < frz_nranges; i++) { if (k >= 0 && frz_ranges[i].lo <= frz_ranges[k].hi) { if (frz_ranges[i].hi > frz_ranges[k].hi) frz_ranges[k].hi = frz_ranges[i].hi; } else frz_ranges[++k] = frz_ranges[i]; }
    frz_nranges = k + 1; uint64_t bytes = 0;
    for (int i = 0; i < frz_nranges; i++) { frz_range *r = &frz_ranges[i]; size_t n = r->hi - r->lo; r->bytes = malloc(n ? n : 1);
        if (!r->bytes || dm_android_read_memory(r->lo, r->bytes, n)) { free(r->bytes); r->bytes = NULL; continue; } bytes += n; }
    LOG("[freeze] %d GPU-written guest ranges, %.1f MiB", frz_nranges, bytes / 1048576.0);
}
typedef struct frz_pool { VkBuffer source, buffer; VkDeviceMemory memory; VkDeviceSize size; struct frz_pool *next; } frz_pool;
static frz_pool *frz_pools;
static void frz_snapshot_pools(void) {
    unsigned count = 0; VkDeviceSize bytes = 0;
    pthread_mutex_lock(&pools_lock);
    for (pool_range *p = pools; p; p = p->next) {
        if (!p->zc_buffer) continue;
        frz_pool *f = calloc(1, sizeof *f); if (!f) die("freeze pool");
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = p->size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        if (vkCreateBuffer(device, &bi, NULL, &f->buffer)) { free(f); continue; }
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, f->buffer, &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++)
            if ((req.memoryTypeBits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
        VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
        if (type == UINT32_MAX || vkAllocateMemory(device, &mi, NULL, &f->memory)) { vkDestroyBuffer(device, f->buffer, NULL); free(f); LOG("[freeze] out of memory after %u pools", count); break; }
        vkBindBufferMemory(device, f->buffer, f->memory, 0);
        f->source = p->zc_buffer; f->size = p->size;
        VkBufferCopy c = {.size = p->size}; vkCmdCopyBuffer(command, f->source, f->buffer, 1, &c);
        f->next = frz_pools; frz_pools = f; count++; bytes += p->size;
    }
    pthread_mutex_unlock(&pools_lock);
    LOG("[freeze] snapshot: %u zero-copy pools, %.1f MiB", count, bytes / 1048576.0);
}

static void frz_copy(const image_entry *e, VkImage src, VkImage dst) {
    VkImageCopy regions[16]; uint32_t n = 0;
    for (uint32_t level = 0; level < e->key.levels && n < 16; level++)
        regions[n++] = (VkImageCopy){.srcSubresource = {e->aspect, level, 0, e->key.layers}, .dstSubresource = {e->aspect, level, 0, e->key.layers},
            .extent = {dimension(e->key.extent.width, level), dimension(e->key.extent.height, level), dimension(e->key.extent.depth, level)}};
    vkCmdCopyImage(command, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, n, regions);
}
static void frz_all_memory_barrier(void) {
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}

void mtl_freeze_release(void);
void mtl_freeze_snapshot(void) {
    mtl_freeze_release(); mtl_finish(); mtl_sub_begin(); graphics_pass_end();
    frz_nranges = 0; frz_noting = 1;
    unsigned count = 0; VkDeviceSize bytes = 0;
    frz_all_memory_barrier();
    for (image_entry *e = image_list; e; e = e->next) {
        if (!e->image || !(e->populated || e->gpu_written)) continue;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = e->key.type, .format = e->key.format, .extent = e->key.extent,
            .mipLevels = e->key.levels, .arrayLayers = e->key.layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .flags = e->key.flags & ~(VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)};
        frz_shadow *s = calloc(1, sizeof *s); if (!s) die("freeze shadow");
        if (vkCreateImage(device, &ci, NULL, &s->image)) { free(s); continue; }
        VkMemoryRequirements req; vkGetImageMemoryRequirements(device, s->image, &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++)
            if ((req.memoryTypeBits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
        VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
        if (type == UINT32_MAX || vkAllocateMemory(device, &mi, NULL, &s->memory)) { vkDestroyImage(device, s->image, NULL); free(s); LOG("[freeze] out of memory after %u images", count); break; }
        vkBindImageMemory(device, s->image, s->memory, 0);
        VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = s->image, .subresourceRange = {e->aspect, 0, e->key.levels, 0, e->key.layers}};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        frz_copy(e, e->image, s->image);
        s->e = e; s->next = frz_shadows; frz_shadows = s; count++; bytes += req.size;
    }
    frz_snapshot_pools();
    frz_all_memory_barrier();
    for (image_entry *e = image_list; e; e = e->next) { e->last_stage = 0; e->last_access = 0; }   /* the barrier above synchronised them all */
    mtl_wait(mtl_sub_commit(), UINT64_MAX);
    LOG("[freeze] snapshot: %u images, %.1f MiB", count, bytes / 1048576.0);
}
/* An image the game frees while a frame is frozen (image_retire_locked; its entry is freed once its fence passes) leaves
   the replay: restoring into it copied into a destroyed image (tu_CmdCopyImage2 on a null image, 3 of 7 freezes on
   2026-10-24). Its shadow stays until mtl_freeze_release, when no restore can still be running. */
static void frz_forget_image(const image_entry *e) {
    for (frz_shadow *s = frz_shadows; s; s = s->next) if (s->e == e) s->e = NULL;
}
void mtl_freeze_restore(void) {
    if (!frz_shadows && !frz_pools) return;
    if (frz_nranges) { mtl_finish();   /* the last replay's write-backs have landed: now overwrite them */
        for (int i = 0; i < frz_nranges; i++) if (frz_ranges[i].bytes) { size_t n = frz_ranges[i].hi - frz_ranges[i].lo; dm_guest_write_prepare((void *)frz_ranges[i].lo, n); memcpy((void *)frz_ranges[i].lo, frz_ranges[i].bytes, n); } }
    mtl_sub_begin(); graphics_pass_end(); frz_all_memory_barrier();
    for (frz_shadow *s = frz_shadows; s; s = s->next) if (s->e) frz_copy(s->e, s->image, s->e->image);
    for (frz_pool *f = frz_pools; f; f = f->next) { VkBufferCopy c = {.size = f->size}; vkCmdCopyBuffer(command, f->buffer, f->source, 1, &c); }
    frz_all_memory_barrier();
    for (image_entry *e = image_list; e; e = e->next) { e->last_stage = 0; e->last_access = 0; }   /* the barrier above synchronised them all */
    mtl_sub_commit();
}
void mtl_freeze_release(void) {
    frz_noting = 0;
    for (int i = 0; i < frz_nranges; i++) free(frz_ranges[i].bytes); frz_nranges = 0;
    if (!frz_shadows && !frz_pools) return;
    mtl_finish();
    while (frz_pools) { frz_pool *f = frz_pools; frz_pools = f->next; vkDestroyBuffer(device, f->buffer, NULL); vkFreeMemory(device, f->memory, NULL); free(f); }
    while (frz_shadows) { frz_shadow *s = frz_shadows; frz_shadows = s->next; vkDestroyImage(device, s->image, NULL); vkFreeMemory(device, s->memory, NULL); free(s); }
}
/* A copy of a submit's uniform capture (mtl_sub_capture_ubos): the replay consumes one per run. */
void *mtl_sub_ucap_clone(const void *p) {
    const ucap *c = p; if (!c) return NULL;
    ucap *d = calloc(1, sizeof *d); if (!d) die("uniform capture clone");
    *d = *c; d->r = NULL; d->off = NULL; d->bytes = NULL; d->buf = (vk_buffer){0}; d->borrowed = 0;   /* a clone is plain memory: its blocks take the arena path */
    if (c->n) {
        size_t total = c->off[c->n - 1] + (size_t)(c->r[c->n - 1].hi - c->r[c->n - 1].lo);
        d->r = malloc(sizeof *d->r * (size_t)c->n); d->off = malloc(sizeof *d->off * (size_t)c->n); d->bytes = malloc(total ? total : 1);
        if (!d->r || !d->off || !d->bytes) die("uniform capture clone");
        ucap_wait(c); memcpy(d->r, c->r, sizeof *d->r * (size_t)c->n); memcpy(d->off, c->off, sizeof *d->off * (size_t)c->n); memcpy(d->bytes, c->bytes, total);
    }
    return d;
}
void mtl_sub_ucap_free(void *p) { ucap_free(p); }

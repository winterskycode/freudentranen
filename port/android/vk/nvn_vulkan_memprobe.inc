#include <sys/mman.h>
/* TOTK_VK_MEMPROBE=1: zero-copy feasibility on the selected driver: a host-visible
   cached allocation's vkMapMemory view is moved with mremap onto a fixed address (as a guest memory pool would be), then
   CPU -> GPU and GPU -> CPU are checked through that address, and CPU bandwidth through the view is compared with plain
   memory. Logs [memprobe] lines and continues. */
static double memprobe_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void memprobe_submit(VkCommandBuffer cb) {
    VkFence f; VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VK_CHECK(vkCreateFence(device, &fi, NULL, &f));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    VK_CHECK(vkQueueSubmit(queue, 1, &si, f)); VK_CHECK(vkWaitForFences(device, 1, &f, VK_TRUE, UINT64_MAX)); vkDestroyFence(device, f, NULL);
}
static void memprobe_run(void) {
    const char *e = getenv("TOTK_VK_MEMPROBE"); if (!e || !atoi(e)) return;
    const VkDeviceSize size = 64ull << 20;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++)
        LOG("[memprobe] memory type %u flags %#x heap %u", i, memory_properties.memoryTypes[i].propertyFlags, memory_properties.memoryTypes[i].heapIndex);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer buf, rb; VK_CHECK(vkCreateBuffer(device, &bi, NULL, &buf)); VK_CHECK(vkCreateBuffer(device, &bi, NULL, &rb));
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(device, buf, &rq);
    const VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount && type == UINT32_MAX; i++)
        if ((rq.memoryTypeBits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & cached) == cached) type = i;
    if (type == UINT32_MAX) { LOG("[memprobe] no host-visible coherent cached type for buffers (bits %#x)", rq.memoryTypeBits); return; }
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = type};
    VkDeviceMemory m, rm; VK_CHECK(vkAllocateMemory(device, &ai, NULL, &m)); VK_CHECK(vkAllocateMemory(device, &ai, NULL, &rm));
    VK_CHECK(vkBindBufferMemory(device, buf, m, 0)); VK_CHECK(vkBindBufferMemory(device, rb, rm, 0));
    void *view; uint32_t *rp; VK_CHECK(vkMapMemory(device, m, 0, VK_WHOLE_SIZE, 0, &view)); VK_CHECK(vkMapMemory(device, rm, 0, VK_WHOLE_SIZE, 0, (void **)&rp));
    /* the "guest pool": a reserved address range, replaced by the moved view */
    void *at = mmap(NULL, rq.size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *fixed = at == MAP_FAILED ? MAP_FAILED : mremap(view, rq.size, rq.size, MREMAP_MAYMOVE | MREMAP_FIXED, at);
    if (fixed == MAP_FAILED) { LOG("[memprobe] type %u: mremap %p -> %p failed: %s", type, view, at, strerror(errno)); return; }
    LOG("[memprobe] type %u: view %p moved to %p (%llu MiB)", type, view, fixed, (unsigned long long)(rq.size >> 20));
    uint32_t *fp = fixed; size_t words = size / 4;
    double t0 = memprobe_ms(); for (size_t i = 0; i < words; i++) fp[i] = (uint32_t)(i * 2654435761u) ^ 0xa5a5a5a5u; double t1 = memprobe_ms();
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; VK_CHECK(vkAllocateCommandBuffers(device, &ca, &cb));
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    VK_CHECK(vkBeginCommandBuffer(cb, &cbi)); vkCmdCopyBuffer(cb, buf, rb, 1, &(VkBufferCopy){0, 0, size});
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL); VK_CHECK(vkEndCommandBuffer(cb)); memprobe_submit(cb);
    size_t bad1 = 0; for (size_t i = 0; i < words; i++) bad1 += rp[i] != fp[i];
    VK_CHECK(vkResetCommandBuffer(cb, 0)); VK_CHECK(vkBeginCommandBuffer(cb, &cbi)); vkCmdFillBuffer(cb, buf, 0, size, 0x5eedf00du);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL); VK_CHECK(vkEndCommandBuffer(cb)); memprobe_submit(cb);
    double t2 = memprobe_ms(); size_t bad2 = 0; for (size_t i = 0; i < words; i++) bad2 += fp[i] != 0x5eedf00du; double t3 = memprobe_ms();
    /* plain memory for comparison */
    /* warm second passes (first touch faults excluded): memset/memcpy as the game's pool writes and our snapshot reads would be */
    uint8_t *plain = malloc(size), *other = malloc(size); memset(plain, 1, size); memset(other, 1, size);
    t0 = memprobe_ms(); memset(fixed, 7, size); t1 = memprobe_ms();
    t2 = memprobe_ms(); memcpy(other, fixed, size); t3 = memprobe_ms();
    double t4 = memprobe_ms(); memset(plain, 7, size); double t5 = memprobe_ms(); memcpy(other, plain, size); double t6 = memprobe_ms(); free(plain); free(other);
    LOG("[memprobe] type %u: CPU->GPU bad %zu, GPU->CPU bad %zu of %zu words => %s; memset %.0f MB/s (plain %.0f), memcpy out of it %.0f MB/s (plain %.0f)",
        type, bad1, bad2, words, !bad1 && !bad2 ? "PASS" : "FAIL", size / 1e3 / (t1 - t0), size / 1e3 / (t5 - t4), size / 1e3 / (t3 - t2), size / 1e3 / (t6 - t5));
    vkFreeCommandBuffers(device, commands, 1, &cb);
    /* method A: export a cached allocation as a dma-buf and map the fd at two addresses (a pool and its alias) */
    if (!ext_memory_fd || !ext_dma_buf) { LOG("[memprobe] dma-buf export extensions not available"); vkFreeCommandBuffers(device, commands, 1, &cb); return; }
    PFN_vkGetMemoryFdKHR get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
    for (uint32_t t = 0; t < memory_properties.memoryTypeCount; t++) {
        VkMemoryPropertyFlags fl = memory_properties.memoryTypes[t].propertyFlags;
        if (!(fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || !(rq.memoryTypeBits & (1u << t))) continue;
        VkExternalMemoryBufferCreateInfo xb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkBufferCreateInfo xbi = bi; xbi.pNext = &xb; VkBuffer xbuf; VK_CHECK(vkCreateBuffer(device, &xbi, NULL, &xbuf));
        VkExportMemoryAllocateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkMemoryAllocateInfo xai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ex, .allocationSize = rq.size, .memoryTypeIndex = t};
        VkDeviceMemory xm; VkResult r = vkAllocateMemory(device, &xai, NULL, &xm);
        if (r) { LOG("[memprobe] A type %u flags %#x: export allocation failed %d", t, fl, r); continue; }
        VK_CHECK(vkBindBufferMemory(device, xbuf, xm, 0));
        VkMemoryGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = xm, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        int fd = -1; r = get_fd ? get_fd(device, &gi, &fd) : VK_ERROR_EXTENSION_NOT_PRESENT;
        if (r || fd < 0) { LOG("[memprobe] A type %u: vkGetMemoryFdKHR failed %d", t, r); continue; }
        uint8_t *v1 = mmap(NULL, rq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0), *v2 = mmap(NULL, rq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        void *slot = mmap(NULL, rq.size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        uint8_t *v3 = slot == MAP_FAILED ? MAP_FAILED : mmap(slot, rq.size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);   /* at a chosen address */
        if (v1 == MAP_FAILED || v2 == MAP_FAILED || v3 == MAP_FAILED) { LOG("[memprobe] A type %u: mmap of the dma-buf failed: %s", t, strerror(errno)); close(fd); continue; }
        uint32_t *w1 = (uint32_t *)v1, *w2 = (uint32_t *)v2, *w3 = (uint32_t *)v3;
        double a0 = memprobe_ms(); for (size_t i = 0; i < words; i++) w1[i] = (uint32_t)(i * 2654435761u) ^ 0x3c3c3c3cu; double a1 = memprobe_ms();
        size_t alias_bad = 0; for (size_t i = 0; i < words; i++) alias_bad += w2[i] != w1[i] || w3[i] != w1[i];
        double a2 = memprobe_ms(); memset(v3, 9, size); double a3 = memprobe_ms(); memset(v3, 0, size);
        for (size_t i = 0; i < words; i++) w3[i] = (uint32_t)(i * 2654435761u) ^ 0x3c3c3c3cu;
        VK_CHECK(vkResetCommandBuffer(cb, 0)); VK_CHECK(vkBeginCommandBuffer(cb, &cbi)); vkCmdCopyBuffer(cb, xbuf, rb, 1, &(VkBufferCopy){0, 0, size});
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL); VK_CHECK(vkEndCommandBuffer(cb)); memprobe_submit(cb);
        size_t g1 = 0; for (size_t i = 0; i < words; i++) g1 += rp[i] != w3[i];
        VK_CHECK(vkResetCommandBuffer(cb, 0)); VK_CHECK(vkBeginCommandBuffer(cb, &cbi)); vkCmdFillBuffer(cb, xbuf, 0, size, 0x0badf00du);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL); VK_CHECK(vkEndCommandBuffer(cb)); memprobe_submit(cb);
        size_t g2 = 0; for (size_t i = 0; i < words; i++) g2 += w1[i] != 0x0badf00du || w2[i] != 0x0badf00du;
        LOG("[memprobe] A dma-buf type %u flags %#x: alias mismatches %zu, CPU(alias)->GPU bad %zu, GPU->CPU(both views) bad %zu => %s; first write %.0f MB/s, warm memset %.0f MB/s",
            t, fl, alias_bad, g1, g2, !alias_bad && !g1 && !g2 ? "PASS" : "FAIL", size / 1e3 / (a1 - a0), size / 1e3 / (a3 - a2));
        munmap(v1, rq.size); munmap(v2, rq.size); munmap(v3, rq.size); close(fd);
        vkDestroyBuffer(device, xbuf, NULL); vkFreeMemory(device, xm, NULL);
    }
    vkFreeCommandBuffers(device, commands, 1, &cb);
}

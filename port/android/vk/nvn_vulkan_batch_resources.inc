// Each ring batch owns its descriptor pools and uniform arena; the caller must complete this slot's GPU fence before selecting/releasing it.
#ifndef DM_VK_BATCH_SLOTS
#define DM_VK_BATCH_SLOTS 8
#endif
#if DM_VK_BATCH_SLOTS < 1 || DM_VK_BATCH_SLOTS > 8
#error DM_VK_BATCH_SLOTS must be between 1 and 8
#endif
#ifndef DM_VK_ARENA_RETAIN_BYTES
/* the uniform arenas of all batch slots (8 x ~18 MiB in the open world; 128 MiB churned chunks) */
#define DM_VK_ARENA_RETAIN_BYTES (384ull << 20)
#endif
typedef struct batch_resources {
    descriptor_chunk *descriptors,*descriptor_current;
    uniform_chunk *uniforms,*uniform_current;
} batch_resources;
static batch_resources batch_resource_slots[DM_VK_BATCH_SLOTS];
static unsigned batch_resource_selected=UINT32_MAX;
static void batch_resources_save(void) {
    if(batch_resource_selected==UINT32_MAX)return;
    batch_resources *r=&batch_resource_slots[batch_resource_selected];
    r->descriptors=descriptor_chunks;r->descriptor_current=descriptor_current;
    r->uniforms=uniform_chunks;r->uniform_current=uniform_current;
}
static void batch_uniform_release(uniform_chunk *chunk) {
    uint64_t capacity=chunk->buffer.capacity;
    if(capacity>immutable_buffer_bytes||capacity>uniform_arena_bytes)die("Vulkan arena ownership accounting");
    immutable_buffer_bytes-=capacity;uniform_arena_bytes-=capacity;buffer_destroy(&chunk->buffer);free(chunk);
}
static void batch_resources_trim_cold(void) {
    // Drop only cold pages in the completed current slot; never overwrite other in-flight batches or discard pages this batch used to meet a budget.
    uniform_chunk **link=&uniform_chunks;
    while(*link){uniform_chunk *chunk=*link;
        if(chunk!=uniform_chunks&&!chunk->used&&uniform_arena_bytes>DM_VK_ARENA_RETAIN_BYTES){*link=chunk->next;batch_uniform_release(chunk);}
        else link=&chunk->next;
    }
    uniform_current=uniform_chunks;
}
static void batch_resources_select(unsigned slot) {
    if(slot>=DM_VK_BATCH_SLOTS)die("Vulkan batch resource slot %u",slot);
    if(batch_resource_selected==UINT32_MAX){
        // Preserve resources created in synchronous mode on the first ring selection; other new slots start with empty pools.
        batch_resource_slots[slot]=(batch_resources){descriptor_chunks,descriptor_current,uniform_chunks,uniform_current};
    }else batch_resources_save();
    batch_resource_selected=slot;batch_resources *r=&batch_resource_slots[slot];
    descriptor_chunks=r->descriptors;descriptor_current=r->descriptor_current;
    uniform_chunks=r->uniforms;uniform_current=r->uniform_current;
    batch_resources_trim_cold();descriptor_reset();texel_views_select(slot);batch_resources_save();
    // descriptor_reset also clears CPU driver, UBO-version and payload indexes; the GPU never reads these CPU workspaces.
}

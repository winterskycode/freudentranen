/* TOTK_VK_BINDLESS=1: sampled textures through global descriptor arrays (plan step 2, "bindless").
   The translator's TOTK mode (MacNativeGames totk_xlate patch 0007, TotkBindless) turns every sampled texture whose
   handle the game binds through the texture constant buffer c[2] into an access of one of the global arrays below,
   indexed by the slot word of that texture in a per-draw slot block, constant buffer c[19]: the array slot of the view
   in the low 20 bits, the sampler's slot above them. Those textures are listed in the shader's .meta as kind 5 entries
   (no binding of their own), word k of the block for the k-th. Per draw, bindless_resolve does for each of them what
   bind_shader does for a texture binding (the handle, the texture, uploads and barriers, the view, the sampler), then
   writes the slots into the stage's block, which uniform_buffer serves as c[19] (a separate block: carried in c[2],
   the slots made every c[2] rebuild copy them twice, +0.5 ms of translation CPU a frame in the still scene).
   Slots belong to our views, not to the game's texture ids: the game re-registers ids while older batches that used
   them are still in flight (TOTK_VK_TEXPROBE, 2026-10-30), but a view never changes, so an array entry is written once
   when its view first needs it and freed with the view (image_destroy_locked runs after the GPU is done with it).
   Samplers are never destroyed: theirs are permanent.
   The global set is set 0 of every pipeline layout (the shaders' own sets are 1 and 2), so it stays bound across
   pipelines; graphics_command_apply binds it once per command buffer, dispatch_compute with every dispatch. */
enum { BL_KINDS = 13, BL_SAMPLER_BINDING = 15, BL_SAMPLERS = 4096, BL_SLOT_BUFFER = 19, BL_BLOCK_WORDS = 64 };
/* array capacity per kind (the translator's binding numbers): 2D, 2D array, 3D, cube, cube array, 1D, 1D array, then
   the shadow forms of 2D, 2D array, cube, cube array, 1D, 1D array */
static const uint32_t bl_capacity[BL_KINDS] = {65536, 8192, 4096, 4096, 1024, 1024, 1024, 8192, 4096, 1024, 1024, 256, 256};
static int bl_state = -1;
static VkDescriptorSetLayout bl_layout; static VkDescriptorPool bl_pool; static VkDescriptorSet bl_set;
static uint32_t *bl_free[BL_KINDS], bl_free_n[BL_KINDS], bl_next[BL_KINDS], bl_live[BL_KINDS];
static pthread_mutex_t bl_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { VkSampler sampler; uint32_t slot; } bl_sampler_entry;
static bl_sampler_entry bl_samplers[BL_SAMPLERS * 2]; static uint32_t bl_sampler_next;
static uint32_t bl_block[6][BL_BLOCK_WORDS]; static unsigned bl_block_words[6];   /* by stage: the slot block of the shader bound last */
static uint64_t bl_resolves, bl_writes;
static int bindless_on(void) {
    if (bl_state < 0) { const char *e = getenv("TOTK_VK_BINDLESS"); bl_state = e && atoi(e) > 0;
        /* bindless translations never mix with the others: the shader cache (DM_VK_SHADERS, read by the shader loader, the
           translator, the pipeline cache and the warm-up) becomes its "-bindless" sibling, before any of them reads it */
        const char *dir = getenv("DM_VK_SHADERS");
        if (bl_state && dir && *dir && !strstr(dir, "-bindless")) { char path[1024], from[1100], to[1100]; snprintf(path, sizeof path, "%s-bindless", dir); mkdir(path, 0700);
            /* the driver check reads the subgroup profile there (dm_vk_driver_validate) */
            snprintf(from, sizeof from, "%s/host-subgroup-size.txt", dir); snprintf(to, sizeof to, "%s/host-subgroup-size.txt", path);
            FILE *in = access(to, F_OK) ? fopen(from, "rb") : NULL;
            if (in) { char b[64]; size_t n = fread(b, 1, sizeof b, in); fclose(in); FILE *out = fopen(to, "wb"); if (out) { fwrite(b, 1, n, out); fclose(out); } }
            setenv("DM_VK_SHADERS", path, 1); }
        LOG("[vulkan-bindless] sampled textures through global descriptor arrays: %s", bl_state ? "on" : "off (TOTK_VK_BINDLESS=1)"); }
    return bl_state;
}
/* The array a shader binding's NVN sampler type reads (the translator's QueryTotkBindlessBinding), or -1. */
static int bl_kind(uint32_t type) {
    int dim = type & 255, k = dim == 3 ? 0 : dim == 4 ? 2 : dim == 5 ? 3 : dim == 1 ? 5 : -1;
    if (k < 0 || (type & 0x200)) return -1;
    if (type & 0x100) k = k == 0 ? 1 : k == 3 ? 4 : k == 5 ? 6 : -1;
    if (k >= 0 && (type & 0x400)) k = k == 0 ? 7 : k == 1 ? 8 : k == 3 ? 9 : k == 4 ? 10 : k == 5 ? 11 : 12;
    return k;
}
/* After the device exists: the global set's layout, pool and set. */
static void bindless_create(void) {
    if (!bindless_on() || bl_set) return;
    VkDescriptorSetLayoutBinding b[BL_KINDS + 1]; VkDescriptorBindingFlags flags[BL_KINDS + 1];
    for (int k = 0; k <= BL_KINDS; k++) {
        int sampler = k == BL_KINDS;
        b[k] = (VkDescriptorSetLayoutBinding){.binding = sampler ? BL_SAMPLER_BINDING : (uint32_t)k, .descriptorType = sampler ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = sampler ? BL_SAMPLERS : bl_capacity[k], .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT};
        flags[k] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = BL_KINDS + 1, .pBindingFlags = flags};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &bf, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = BL_KINDS + 1, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &bl_layout));
    uint32_t images = 0; for (int k = 0; k < BL_KINDS; k++) images += bl_capacity[k];
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, images}, {VK_DESCRIPTOR_TYPE_SAMPLER, BL_SAMPLERS}};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
    VK_CHECK(vkCreateDescriptorPool(device, &pi, NULL, &bl_pool));
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = bl_pool, .descriptorSetCount = 1, .pSetLayouts = &bl_layout};
    VK_CHECK(vkAllocateDescriptorSets(device, &ai, &bl_set));
    for (int k = 0; k < BL_KINDS; k++) { bl_free[k] = malloc(bl_capacity[k] * sizeof *bl_free[k]); if (!bl_free[k]) die("bindless slots"); }
    LOG("[vulkan-bindless] global set: %u image slots in %d arrays, %u samplers", images, BL_KINDS, BL_SAMPLERS);
}
/* The view's slot in array `kind`, written into the array the first time (slot + 1 is kept in the view). */
static uint32_t bl_view_slot(cached_image_view *v, int kind) {
    if (v->bl_slot[kind]) return v->bl_slot[kind] - 1;
    pthread_mutex_lock(&bl_lock);
    uint32_t slot = bl_free_n[kind] ? bl_free[kind][--bl_free_n[kind]] : bl_next[kind] < bl_capacity[kind] ? bl_next[kind]++ : UINT32_MAX;
    if (slot != UINT32_MAX) bl_live[kind]++;
    pthread_mutex_unlock(&bl_lock);
    if (slot == UINT32_MAX) die("bindless: array %d full (%u views)", kind, bl_capacity[kind]);
    VkDescriptorImageInfo ii = {.imageView = v->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = bl_set, .dstBinding = (uint32_t)kind, .dstArrayElement = slot, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii};
    vkUpdateDescriptorSets(device, 1, &w, 0, NULL); bl_writes++;
    v->bl_slot[kind] = slot + 1; return slot;
}
/* image_destroy_locked: the GPU is done with the image, so its views' slots are free again */
static void bindless_view_forget(cached_image_view *v) {
    if (!bl_set) return;
    pthread_mutex_lock(&bl_lock);
    for (int k = 0; k < BL_KINDS; k++) if (v->bl_slot[k]) { bl_free[k][bl_free_n[k]++] = v->bl_slot[k] - 1; bl_live[k]--; v->bl_slot[k] = 0; }
    pthread_mutex_unlock(&bl_lock);
}
static uint32_t bl_sampler_slot(VkSampler sampler) {
    uint32_t i = (uint32_t)(((uintptr_t)sampler >> 4) * 0x9E3779B1u) & (BL_SAMPLERS * 2 - 1);
    while (bl_samplers[i].sampler && bl_samplers[i].sampler != sampler) i = (i + 1) & (BL_SAMPLERS * 2 - 1);
    if (bl_samplers[i].sampler) return bl_samplers[i].slot;
    if (bl_sampler_next >= BL_SAMPLERS) die("bindless: more than %u samplers", BL_SAMPLERS);
    uint32_t slot = bl_sampler_next++;
    VkDescriptorImageInfo ii = {.sampler = sampler};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = bl_set, .dstBinding = BL_SAMPLER_BINDING, .dstArrayElement = slot, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(device, 1, &w, 0, NULL); bl_writes++;
    bl_samplers[i] = (bl_sampler_entry){sampler, slot}; return slot;
}
static cached_image_view *descriptor_view_entry(const texture_descriptor *descriptor, image_entry *e, uint32_t type, int storage);
/* bind_shader, before the shader's bindings: each kind 5 entry resolved as a combined texture binding would be, its
   slots written into the stage's slot table */
static void bindless_resolve(shader_entry *s) {
    if (!s->bindless_count || s->stage >= 6) return;
    if (s->bindless_count > BL_BLOCK_WORDS) die("bindless: shader %s reads %u textures", s->key, s->bindless_count);
    uint32_t *block = bl_block[s->stage]; bl_block_words[s->stage] = s->bindless_count;
    for (unsigned i = 0; i < s->bindless_count; i++) {
        const shader_binding *b = &s->bindless[i]; int kind = bl_kind(b->type);
        if (kind < 0) die("bindless: shader %s entry type %#x handle %#x", s->key, b->type, b->handle);
        uint32_t handle = shader_handle(b, s->stage, 0);
        texture_descriptor d = descriptor_texture(handle & 0xfffff);
        if (handle && !d.valid) gpu_unregistered_textures++;
        if (d.valid && !descriptor_type_fits(&d.texture, b->type)) d.valid = 0;   /* the stand-in of the declared type */
        image_entry *e = descriptor_image(&d, b->type, 0, shader_pipeline_stage(s), VK_ACCESS_SHADER_READ_BIT);
        cached_image_view *v = descriptor_view_entry(&d, e, b->type, 0);
        uint32_t slots = bl_view_slot(v, kind) | bl_sampler_slot(sampler_get_for_type((handle >> 20) & 0xfff, b->type)) << 20;
        block[i] = slots;
        bl_resolves++;
    }
    static uint64_t last; if (presents >= last + 600) { if (last) { uint32_t live = 0; for (int k = 0; k < BL_KINDS; k++) live += bl_live[k];
        LOG("[vulkan-bindless] %.0f textures resolved a frame, %.1f array writes a frame, %u live view slots, %u samplers", bl_resolves / 600.0, bl_writes / 600.0, live, bl_sampler_next); }
        last = presents; bl_resolves = bl_writes = 0; }
}

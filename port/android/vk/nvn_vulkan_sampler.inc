// Shared sampler objects are owned by effective parameter keys; slots retain references, and re-registration never destroys in-flight objects.
typedef struct {
    uint32_t mag,min,mip,u,v,w,compare,compare_op,border;
    /* aniso: 0 = off */
    float bias,min_lod,max_lod,aniso;
} sampler_key;
typedef struct sampler_cache_entry { sampler_key key;VkSampler sampler;struct sampler_cache_entry *next; } sampler_cache_entry;
static sampler_cache_entry *sampler_cache[256];
/* TOTK_ANISO (sampler_key_make) */
static float sampler_aniso=-1;
static uint64_t sampler_creations,sampler_reuses;
static void sampler_resolved_locked(int id,sampler_descriptor *descriptor,VkSampler sampler) {
    if(descriptor)descriptor->sampler=sampler;
    if(descriptor&&id>=0&&id<4096)atomic_store_explicit(&sampler_fast[id],(uintptr_t)sampler,memory_order_release);
}
/* TOTK_ANISO changed: every id resolves again through the new key */
static void sampler_forget_all_locked(void) {
    for(int i=0;i<sampler_capacity;i++)sampler_descriptors[i].sampler=VK_NULL_HANDLE;
    for(int i=0;i<4096;i++)atomic_store_explicit(&sampler_fast[i],0,memory_order_relaxed);
}
static VkBorderColor sampler_border(const samp_par *p) {
    // Match the Metal backend's three standard border colors; arbitrary NVN custom colors remain limited.
    return p->border[3]>.5f?(p->border[0]>.5f?VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK):VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
}
static sampler_key sampler_key_make(const samp_par *p) {
    sampler_key key={.mag=p->magf?VK_FILTER_LINEAR:VK_FILTER_NEAREST,.min=(p->minf&1)?VK_FILTER_LINEAR:VK_FILTER_NEAREST,
        .mip=p->minf>=4?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST,.u=wrap_mode(p->ws),.v=wrap_mode(p->wt),.w=wrap_mode(p->wr),
        .compare=!!p->cmp_mode,.compare_op=p->cmp_mode&&p->cmp_func?p->cmp_func-1:VK_COMPARE_OP_ALWAYS,.border=sampler_border(p),.bias=p->lod_bias};
    if(p->minf>=2){key.min_lod=p->lod_min>0?p->lod_min:0;key.max_lod=p->lod_max>=key.min_lod?p->lod_max:key.min_lod;}
    if(key.u!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER&&key.v!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER&&key.w!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER)key.border=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if(key.bias==0)key.bias=0;
    /* NVN min/max reduction (1 MIN, 2 MAX) is not applied: count the samplers that ask for it */
    { static unsigned reduction_notes;if(p->reduction&&reduction_notes<16){reduction_notes++;
        LOG("[vulkan-sampler] reduction %u requested (ignored): min %u mag %u wrap %u,%u,%u compare %u",(unsigned)p->reduction,(unsigned)p->minf,(unsigned)p->magf,(unsigned)p->ws,(unsigned)p->wt,(unsigned)p->wr,(unsigned)p->cmp_mode);} }
    // Mirror-clamp/anisotropy retain the existing capability limits; never use Vulkan features that were not enabled.
    /* Anisotropic filtering: the game's own level (TOTK asks for 2x on some samplers), clamped to the device.
       TOTK_ANISO=<n> forces n on trilinear samplers only (NVN LINEAR_MIPMAP_LINEAR), as Ryujinx forces its setting,
       so post-processing and UI samplers keep what the game set; 1 turns those off, 0 = the game's. The in-game menu
       changes it live (sampler_aniso): the level is part of the key, so the next lookups make new samplers. */
    { if(sampler_aniso<0){const char *x=getenv("TOTK_ANISO");sampler_aniso=x?(float)atof(x):0;}
      float forced=sampler_aniso,level=p->aniso;if(forced>=1&&p->minf==5)level=forced;
      if(level>1&&gpu_features.samplerAnisotropy)key.aniso=level<gpu_limits.maxSamplerAnisotropy?level:gpu_limits.maxSamplerAnisotropy; }
    return key;
}
static unsigned sampler_key_bucket(const sampler_key *key) {
    const uint8_t *bytes=(const uint8_t*)key;uint64_t hash=14695981039346656037ull;
    for(unsigned i=0;i<sizeof *key;i++)hash=(hash^bytes[i])*1099511628211ull;return hash&255;
}
static void sampler_compare_note_locked(int id,uint32_t type,const sampler_descriptor *descriptor) {
    if(!(type&255)||!descriptor)return;
    static unsigned notes;const samp_par *parameters=&descriptor->parameters;
    if(notes<12&&!!(type&1024)!=!!parameters->cmp_mode){notes++;LOG("[vulkan-sampler] compare mismatch id=%d reflected_shadow=%u registered_compare=%u func=%u",id,!!(type&1024),!!parameters->cmp_mode,(unsigned)parameters->cmp_func);}
}
static VkSampler sampler_get_for_type(int id,uint32_t type) {
    if(id>=0&&id<4096){uintptr_t s=atomic_load_explicit(&sampler_fast[id],memory_order_acquire);if(s)return (VkSampler)s;}
    pthread_mutex_lock(&descriptor_lock);sampler_descriptor *descriptor=id>=0&&id<sampler_capacity&&sampler_descriptors[id].valid?&sampler_descriptors[id]:NULL;
    sampler_compare_note_locked(id,type,descriptor);
    if(descriptor&&descriptor->sampler){VkSampler sampler=descriptor->sampler;sampler_reuses++;pthread_mutex_unlock(&descriptor_lock);return sampler;}
    samp_par parameters=descriptor?descriptor->parameters:(samp_par){.lod_max=1000};sampler_key key=sampler_key_make(&parameters);unsigned bucket=sampler_key_bucket(&key);
    for(sampler_cache_entry *entry=sampler_cache[bucket];entry;entry=entry->next)if(!memcmp(&entry->key,&key,sizeof key)){
        sampler_resolved_locked(id,descriptor,entry->sampler);sampler_reuses++;pthread_mutex_unlock(&descriptor_lock);return entry->sampler;
    }
    sampler_cache_entry *entry=calloc(1,sizeof *entry);if(!entry)die("Vulkan sampler parameter cache allocation");entry->key=key;
    VkSamplerCreateInfo info={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=key.mag,.minFilter=key.min,.mipmapMode=key.mip,
        .addressModeU=key.u,.addressModeV=key.v,.addressModeW=key.w,.mipLodBias=key.bias,.compareEnable=key.compare,.compareOp=key.compare_op,
        .minLod=key.min_lod,.maxLod=key.max_lod,.borderColor=key.border,.anisotropyEnable=key.aniso>1,.maxAnisotropy=key.aniso>1?key.aniso:1};
    VK_CHECK(vkCreateSampler(device,&info,NULL,&entry->sampler));
    { static uint64_t aniso_samplers;if(key.aniso>1&&++aniso_samplers<=8)LOG("[vulkan-sampler] anisotropic sampler %llu: %gx (min %u mip %u)",(unsigned long long)aniso_samplers,key.aniso,key.min,key.mip); }
    entry->next=sampler_cache[bucket];sampler_cache[bucket]=entry;sampler_creations++;
    sampler_resolved_locked(id,descriptor,entry->sampler);
    static unsigned border_notes;if(border_notes<12&&(key.u==VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER||key.v==VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER||key.w==VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER)){
        border_notes++;LOG("[vulkan-sampler] id=%d wrap=%u,%u,%u border=%g,%g,%g,%g canonical=%u compare=%u/%u",id,(unsigned)parameters.ws,(unsigned)parameters.wt,(unsigned)parameters.wr,parameters.border[0],parameters.border[1],parameters.border[2],parameters.border[3],key.border,key.compare,key.compare_op);
    }
    pthread_mutex_unlock(&descriptor_lock);return entry->sampler;
}

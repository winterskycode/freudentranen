// 预编译 SPIR-V、描述符绑定及计算提交。| Precompiled SPIR-V, descriptor binding and compute submission.
typedef struct { uint32_t kind,binding,slot,cb_slot,cb_offset,handle,type,count,flags,separate; } shader_binding;
typedef struct { VkDescriptorType type; uint32_t offset; int storage_slot,uniform_pointer,dynamic_index; int cb_pointer,cb_pointer_offset; } shader_binding_plan;

#include "nvn_vulkan_spirv_link.inc"
/* per-binding memos of the last draw's answers (nvn_vulkan_binding_cache.inc, nvn_vulkan_forward_bind.inc) */
typedef struct { uint64_t address, serial; int index; } ucap_memo_entry;   /* the submit capture's range holding this binding's block */
typedef struct { uint64_t address, bytes, gen_all, gen_unknown, pools; VkDescriptorBufferInfo info; int valid;
    uintptr_t dep_address; uint64_t dep_bytes, dep_gen_unknown, dep_gen_all; int dep_valid, dep_result; } fwd_memo;   /* a read-only buffer bound in place; dep_*: the dependency pass's answer for it */
/* The translator puts the support buffer at set 0, binding 0 in every stage, so a fragment shader (set 1) read it from the
   vertex shader's set. Its own set already has the binding (the .meta lists it and bind_shader writes the same global
   buffer there), so with pipeline libraries (gpl_state) its decorations of set 0 become its own set: a fragment shader
   library then depends on the fragment shader alone (nvn_vulkan_gpl.inc). */
static void spirv_own_set(uint32_t *code, size_t words, uint32_t set) {
    if (words < 5 || !set) return;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) return;
        if (op == 71 && n == 4 && code[i + 2] == 34 && code[i + 3] == 0) code[i + 3] = set;   /* OpDecorate <id> DescriptorSet 0 */
        i += n; }
}
typedef struct shader_entry {
    char key[17]; uint32_t stage,set,count,c1_size,attributes[32];
    shader_binding *bindings; uint8_t *c1; VkShaderModule module; VkDescriptorSetLayout layout;
    shader_binding *bindless; unsigned bindless_count;   /* kind 5: textures read through the global arrays (nvn_vulkan_bindless.inc), after bindings[count] */
    shader_binding_plan *binding_plan; uint32_t buffer_count,image_count,texel_count,dynamic_count;
    VkPipelineLayout compute_layout; VkPipeline compute_pipeline;
    vk_buffer constant_buffer;
    /* TOTK stage linking (nvn_vulkan_spirv_link.inc): vertex outputs / fragment inputs by location, the vertex shader's
       SPIR-V, and its variants with outputs added for fragment shaders that read more */
    uint32_t io_mask, frag_out_mask; int control_dumped, early_tests;   /* early_tests: built with EarlyFragmentTests (shader_enable_early_tests) */ spirv_io io[32]; uint32_t *spirv; size_t spirv_words;
    struct linked_module { uint32_t missing; VkShaderModule module; struct linked_module *next; } *linked;
    struct shader_entry *next, *hash_next;
    uint32_t *ubo_bytes;   /* per binding: bytes of a uniform buffer the SPIR-V can read (0: unknown, read the bound range) */
    /* per image descriptor (binding_plan offset + element): what the last draw resolved the game's handle to. Valid while
       the handle, the texture table entry (texture_seq) and the image table (image_generation) are unchanged; the hazard
       and upload work (descriptor_image_sync) still runs every draw. */
    struct tex_memo { uint32_t handle, seq; uint64_t gen; struct image_entry *e; VkImageView view; texture_descriptor d;
        uint32_t dep_handle, dep_seq; uint64_t dep_gen; int dep_valid, dep_unknown; } *tmemo;   /* dep_*: dependencies_shader_unknown's answer for this handle while pp_unknown is unchanged */
    /* the descriptor infos the last draw of this shader resolved in this batch, and the set they got: a draw that resolves
       the same infos (most draws: only uniform offsets change, and those are dynamic) reuses the set without the payload hash */
    uint64_t dmemo_batch; VkDescriptorSet dmemo_set; uint8_t *dmemo_infos;
    ucap_memo_entry *umemo;   /* per binding (count) */
    fwd_memo *bmemo;          /* per buffer descriptor (buffer_count) */
    char game_id[17];      /* TOTK: the game shader's id (first 16 hex digits of its SHA-256), as the Mac port names shaders */
    uint64_t layout_hash;  /* its descriptor set layout's definition (nvn_vulkan_gpl.inc: identically defined layouts are interchangeable) */
} shader_entry;
static void graphics_probe_after_draw(const char *fs_key);
static void graphics_probe_cs_inputs(const struct shader_entry *shader,const VkWriteDescriptorSet *writes,uint32_t count,const uint32_t *dynamic_offsets,unsigned dynamic_count);
static void graphics_probe_cs_outputs(const struct shader_entry *shader);
static shader_entry *shader_list;
/* keys are 16 hex digits: bucket by the first 12 bits (the list walk with strcmp over ~3800 shaders was ~5% of the translation thread) */
static shader_entry *shader_hash[4096];
static unsigned shader_key_bucket(const char *key) { unsigned h=0; for(int i=0;i<3&&key[i];i++){char c=key[i]; h=h*16+(unsigned)(c<='9'?c-'0':(c|32)-'a'+10);} return h&4095; }
// 提交线程与预热线程共享着色器/通道/布局表；这把锁串行化表的增查，预热对象因此直接进入运行期缓存。
// The submit and warm threads share the shader/pass/layout tables; this lock serializes table access so warmed objects land directly in the runtime caches.
static pthread_mutex_t graphics_object_lock=PTHREAD_MUTEX_INITIALIZER;
// 只计真实创建，缓存命中不读时钟；每类每60帧最多4条慢创建明细。
// Time actual creation only, never cache hits; cap slow details at four per category per 60 frames.
typedef struct resource_create_start {uint64_t wall,cpu;} resource_create_start;
typedef struct resource_create_stats {uint64_t count,wall,cpu,slow,max_wall,max_cpu,max_wall_frame,max_cpu_frame,window,logged;} resource_create_stats;
static resource_create_stats resource_creates[3];
// NVN 长帧账目用的累计值：真实创建条数与墙钟纳秒，永不按窗口清零（调用方取差）。
// Cumulative totals for the NVN long-frame accounting: genuine creation counts and wall nanoseconds, never reset per window (the caller diffs them).
static uint64_t resource_created_count[3],resource_created_ns[3];
// 预热线程不记账：后台创建的成本不属于任何一帧，预热自报 [vulkan-pso-warm] 明细。
// The warm thread does not account: background creation cost belongs to no frame; the warm reports its own [vulkan-pso-warm] details.
static _Thread_local int resource_create_quiet;
struct shader_entry;static void pso_record_compute(const struct shader_entry *cs);   /* nvn_vulkan_warm.inc */
static const char *resource_create_names[]={"shader_load","graphics_pipeline","compute_pipeline"};
static resource_create_start resource_create_begin(void) {
    return (resource_create_start){dm_android_clock_ns(CLOCK_MONOTONIC),dm_android_clock_ns(CLOCK_THREAD_CPUTIME_ID)};
}
static void resource_create_end(unsigned kind,resource_create_start start,const char *shader,const char *fragment) {
    if(resource_create_quiet)return;
    uint64_t cpu=dm_android_clock_ns(CLOCK_THREAD_CPUTIME_ID)-start.cpu,wall=dm_android_clock_ns(CLOCK_MONOTONIC)-start.wall,frame=presents+1;
    resource_create_stats *s=&resource_creates[kind];s->count++;s->wall+=wall;s->cpu+=cpu;
    resource_created_count[kind]++;resource_created_ns[kind]+=wall;
    if(wall>s->max_wall){s->max_wall=wall;s->max_wall_frame=frame;}if(cpu>s->max_cpu){s->max_cpu=cpu;s->max_cpu_frame=frame;}
    if(wall<=2000000&&cpu<=2000000)return;s->slow++;
    uint64_t window=(frame-1)/60;if(s->window!=window){s->window=window;s->logged=0;}
    if(s->logged++<4)LOG("[vulkan-create-slow] frame=%llu kind=%s wall_ms=%.3f thread_cpu_ms=%.3f shader=%s fragment=%s scope=cache_miss_creation",(unsigned long long)frame,resource_create_names[kind],wall/1e6,cpu/1e6,shader,fragment?fragment:"none");
}
static void resource_create_report(void) {
    for(unsigned i=0;i<3;i++){resource_create_stats *s=&resource_creates[i];
        LOG("[vulkan-create-window] frame=%llu kind=%s count=%llu slow_over2ms=%llu wall_ms=%.3f thread_cpu_ms=%.3f max_wall_ms=%.3f max_wall_frame=%llu max_cpu_ms=%.3f max_cpu_frame=%llu",(unsigned long long)presents,resource_create_names[i],(unsigned long long)s->count,(unsigned long long)s->slow,s->wall/1e6,s->cpu/1e6,s->max_wall/1e6,(unsigned long long)s->max_wall_frame,s->max_cpu/1e6,(unsigned long long)s->max_cpu_frame);
        s->count=s->wall=s->cpu=s->slow=s->max_wall=s->max_cpu=s->max_wall_frame=s->max_cpu_frame=0;
    }
}
#include "nvn_vulkan_handles.inc"
// 离线翻译选项必须匹配实际 GPU 分组；独立 GLSL 测试和旧缓存的未知配置明确记录。
// Offline translation options must match actual GPU subgroups; explicitly record unknown profiles of independent GLSL tests and legacy caches.
static void shader_profile_check(void) {
    static int checked;if(checked)return;checked=1;
    const char *dir=getenv("DM_VK_SHADERS");if(!dir)die("Set DM_VK_SHADERS to Vulkan shader cache");
    char path[1024];snprintf(path,sizeof path,"%s/host-subgroup-size.txt",dir);FILE *file=fopen(path,"r");
    if(!file){if(errno!=ENOENT)die("Vulkan shader profile open");LOG("[vulkan] shader subgroup profile unknown; shader execution subgroup=%u",gpu_subgroup_size);return;}
    char data[32];size_t n=fread(data,1,sizeof data-1,file);int extra=fgetc(file),error=ferror(file);fclose(file);data[n]=0;
    errno=0;char *end;unsigned long size=strtoul(data,&end,10);while(*end==' '||*end=='\n'||*end=='\r'||*end=='\t')end++;
    if(error||extra!=EOF||errno||end==data||*end||!size||size>128||(size&(size-1)))die("Invalid Vulkan shader subgroup profile");
    if(size!=gpu_subgroup_size)die("Vulkan shader subgroup profile %lu differs from selected shader execution %u; rebuild for the actual policy",size,gpu_subgroup_size);
    LOG("[vulkan] shader subgroup profile=%lu matches selected shader execution",size);
}
static void *read_shader_file(const char *key,const char *suffix,size_t *size) {
    shader_profile_check();
    const char *dir=getenv("DM_VK_SHADERS");if(!dir)die("Set DM_VK_SHADERS to Vulkan shader cache");
    char path[1024];snprintf(path,sizeof path,"%s/%s.%s",dir,key,suffix);FILE *f=fopen(path,"rb");if(!f)die("Missing Vulkan shader %s",path);
    if(fseek(f,0,SEEK_END))die("Shader seek");long n=ftell(f);if(n<0||n>32*1024*1024)die("Shader size");rewind(f);void *data=malloc(n);if(!data||fread(data,1,n,f)!=(size_t)n)die("Shader read");fclose(f);*size=n;return data;
}
// 预热路径的非致命读取：清单可能引用已不在本机着色器缓存里的键，缺失按跳过处理。
// Non-fatal read for the warm path: manifests may reference keys no longer present in the local shader cache; missing files are skipped.
static void *read_shader_file_opt(const char *key,const char *suffix,size_t *size) {
    shader_profile_check();
    const char *dir=getenv("DM_VK_SHADERS");if(!dir)return NULL;
    char path[1024];snprintf(path,sizeof path,"%s/%s.%s",dir,key,suffix);FILE *f=fopen(path,"rb");if(!f)return NULL;
    if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}long n=ftell(f);if(n<0||n>32*1024*1024){fclose(f);return NULL;}rewind(f);void *data=malloc(n?n:1);if(!data||fread(data,1,n,f)!=(size_t)n){free(data);fclose(f);return NULL;}fclose(f);*size=n;return data;
}
// 只有内容变化就换 arena 槽位的客体 UBO 用动态偏移；支持数据、c1 与 c0/c2 驱动表槽位跨绘制稳定。
// Only guest UBOs that migrate arena slots as their contents change use dynamic offsets; the support
// data, c1 and the c0/c2 driver table slots stay put across draws.
// 客体 UBO 随内容变化，是换槽位最频繁的一档，动态偏移优先给它。
// Guest UBOs change with content and move slots most often, so they get dynamic offsets first.
static int descriptor_uniform_dynamic_guest(const shader_binding *b) {
    return b->kind==1&&b->slot>=3&&b->slot<19;
}
// c0/c2 驱动表也由 arena 槽位承载，位置跨绘制同样会变；c1 与支持数据位置固定，不用动态偏移。
// The c0/c2 driver tables also live in arena slots and move across draws; c1 and the support data keep fixed
// positions and need no dynamic offset.
static int descriptor_uniform_dynamic(const shader_binding *b) {
    return b->kind==1&&(b->slot==0||b->slot==2||(b->slot>=3&&b->slot<19)||b->slot==19);   /* 19: the bindless slot block */
}
// 静态与动态 uniform 共用一条读取与依赖路径，区别只在描述符里的位置由谁提供。
// Static and dynamic uniforms share one read and dependency path; only who supplies the descriptor position differs.
static int descriptor_type_uniform(VkDescriptorType type) {
    return type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
}
static VkDescriptorType descriptor_type(const shader_binding *b) {
    if(b->kind<=1)return descriptor_uniform_dynamic(b)?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if(b->kind==2)return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if(b->kind==4)return (b->type&255)==2?VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if((b->type&255)==2)return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    if(b->separate)return (b->type&255)==0?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}
/* Uniform-buffer extents: Ryujinx declares every constant buffer as a Uniform block with one float4[4096] member, read
   through OpAccessChain(var, member, element[, component]) or a member pointer indexed later. When every element index is
   an OpConstant the shader reads (max + 1) * 16 bytes; any other use of the block (or of a pointer derived from it) or a
   computed index leaves the extent unknown (0). Bound uniform ranges average ~9.5 KB in the cave while the median shader
   reads 176 bytes. */
static uint32_t spirv_uniform_extent(const uint32_t *w,size_t words,uint32_t var){
    enum {CAP=64};uint32_t derived[CAP];unsigned nd=0;int64_t max=-1;
    /* pass 1: constants are needed by id; collect them lazily with a second scan per index (shaders are small) */
    for(int pass=0;pass<2;pass++)
    for(size_t i=5;i<words;){uint32_t op=w[i]&0xffff,n=w[i]>>16;if(!n||i+n>words)return 0;const uint32_t *a=w+i+1;
        if((op==65||op==66)&&n>=4){   /* OpAccessChain / OpInBoundsAccessChain: type result base idx... */
            uint32_t base=a[2];int from_var=base==var,from_derived=0;for(unsigned k=0;k<nd;k++)if(derived[k]==base)from_derived=1;
            if(from_var||from_derived){
                unsigned element_at=from_var?4:3;   /* operand index of the array element */
                if(n-1<=element_at){if(from_var&&n-1==4){if(pass==0){if(nd==CAP)return 0;derived[nd++]=a[1];}}else return 0;}   /* member pointer: index it later */
                else if(pass==1){uint32_t e=a[element_at];int found=0;
                    for(size_t j=5;j<words;){uint32_t op2=w[j]&0xffff,n2=w[j]>>16;if(!n2)return 0;if(op2==43&&n2>=4&&w[j+2]==e){if(w[j+3]>4095)return 0;if((int64_t)w[j+3]>max)max=w[j+3];found=1;break;}j+=n2;}
                    if(!found)return 0;}
                for(unsigned k=element_at+1;k<n-1;k++)if(a[k]==var)return 0;
            }else for(unsigned k=3;k<n-1;k++){if(a[k]==var)return 0;for(unsigned d=0;d<nd;d++)if(a[k]==derived[d])return 0;}
        }else if(op!=71&&op!=72&&op!=5&&op!=6&&op!=59&&op!=15&&pass==1){   /* any other reference: unknown */
            for(unsigned k=0;k+1<n;k++){if(a[k]==var)return 0;for(unsigned d=0;d<nd;d++)if(a[k]==derived[d])return 0;}
        }
        i+=n;}
    return max<0?16:(uint32_t)(max+1)*16;
}
static void spirv_uniform_extents(shader_entry *s,const uint32_t *w,size_t words){
    s->ubo_bytes=calloc(s->count?s->count:1,sizeof *s->ubo_bytes);if(!s->ubo_bytes)die("Vulkan uniform extents");
    uint32_t vars[64],binds[64];unsigned nv=0;
    for(size_t i=5;i<words;){uint32_t op=w[i]&0xffff,n=w[i]>>16;if(!n||i+n>words)return;
        if(op==59&&n>=4&&w[i+3]==2&&nv<64)vars[nv++]=w[i+2];   /* OpVariable type id Uniform */
        i+=n;}
    for(unsigned v=0;v<nv;v++){binds[v]=UINT32_MAX;
        for(size_t i=5;i<words;){uint32_t op=w[i]&0xffff,n=w[i]>>16;if(!n)return;if(op==71&&n>=4&&w[i+1]==vars[v]&&w[i+2]==33){binds[v]=w[i+3];break;}i+=n;}}
    for(unsigned b=0;b<s->count;b++)for(unsigned v=0;v<nv;v++)if(binds[v]==s->bindings[b].binding){s->ubo_bytes[b]=spirv_uniform_extent(w,words,vars[v]);break;}
}
/* NVN's early fragment tests (a fragment shader's control section, word 0x718 bit 0; GLSL layout(early_fragment_tests)): the
   depth/stencil test runs before the shader, so a fragment that fails it has no side effects. Vulkan runs the test after
   a shader with storage writes unless the module declares EarlyFragmentTests, so without it every covered fragment
   stored. TOTK marks its 64x64 shadow tiles that hold sky with a full-screen triangle tested EQUAL against the far depth;
   every tile got marked, and lit tiles that went through the sky path came out as dark squares (2026-10-24; Ryujinx
   passes the register as EarlyZForce). Returns the patched words, or NULL when the module already has the mode or
   replaces depth (the hardware then tests late anyway). */
static uint32_t *spirv_add_early_tests(const uint32_t *code,size_t words,size_t *out_words) {
    size_t at=0;uint32_t entry=0;
    for(size_t i=5;i<words;){uint32_t op=code[i]&0xffff,n=code[i]>>16;if(!n||i+n>words)return NULL;
        if(op==15&&n>=3&&!at){entry=code[i+2];at=i+n;}   /* OpEntryPoint: the mode goes right after it */
        if(op==16&&n>=3&&(code[i+2]==9||code[i+2]==12))return NULL;   /* EarlyFragmentTests already, or DepthReplacing */
        i+=n;}
    if(!at)return NULL;
    uint32_t *out=malloc((words+3)*4);if(!out)die("early fragment tests patch allocation");
    memcpy(out,code,at*4);out[at]=(3u<<16)|16;out[at+1]=entry;out[at+2]=9;memcpy(out+at+3,code+at,(words-at)*4);
    *out_words=words+3;return out;
}
/* shader_load_private (this thread): a shader not loaded yet is built but not published -- the library warm-up
   (nvn_vulkan_gpl.inc) builds one library from it and frees it (shader_entry_free_private); the published entry when
   there is one, flagged by shader_load_was_private = 0. */
static __thread int shader_load_private, shader_load_was_private;
static shader_entry *shader_load_by_key(const char *key,int stats,int fatal) {
    shader_load_was_private=0;
    pthread_mutex_lock(&graphics_object_lock);
    for(shader_entry *s=shader_hash[shader_key_bucket(key)];s;s=s->hash_next)if(!strcmp(s->key,key)){pthread_mutex_unlock(&graphics_object_lock);return s;}
    /* Loaded without the lock (file reads, parsing and driver objects take 1-10 ms; the translation thread takes this
       lock for every draw's layout lookup, and the warm thread preloads the whole cache), published under it below; a
       shader another thread published meanwhile wins and this copy is dropped. */
    pthread_mutex_unlock(&graphics_object_lock);
    resource_create_start creation=resource_create_begin();
    // 清单列出的键（驱动变体）在任何路径上都必须完整：.meta 也致命读取与校验。| A manifest-listed key must be complete on every path: its .meta is read and checked fatally too.
    const shader_variant_key *variant=shader_variant_lookup(key);int required=fatal||variant!=NULL;
    size_t bytes;uint32_t *meta=required?read_shader_file(key,"meta",&bytes):read_shader_file_opt(key,"meta",&bytes);
    if(!meta){return NULL;}
    if(bytes<28||memcmp(meta,"DMVK",4)||meta[1]!=1||meta[4]>256||meta[5]>65536||meta[6]!=32||bytes!=28+meta[4]*40+meta[5]+128){
        if(required)die("Invalid Vulkan reflection %s",key);
        free(meta);return NULL;
    }
    // 驱动变体只替换 SPIR-V 本体；反射 .meta 共用（构建期已核对接口一致）。清单列出的键在预热路径也是致命读取：
    // 变体缺失、或变体/基础模块/.meta 与清单记录的那一代不符都终止，绝不退回基础模块（见 nvn_vulkan_shader_variant.inc）。
    // A driver variant replaces only the SPIR-V; the .meta reflection is shared (interface equality checked at build time).
    // A key the manifest lists is read fatally on the warm path too: a missing variant, or a variant / base module / .meta
    // that is not the generation the manifest recorded, aborts and never falls back to the base module.
    void *code;
    if(variant){size_t meta_bytes=bytes;code=shader_variant_load(variant,key,meta,meta_bytes,&bytes,read_shader_file);}
    else{
        code=fatal?read_shader_file(key,"spv",&bytes):read_shader_file_opt(key,"spv",&bytes);
        if(!code){free(meta);return NULL;}
    }
    shader_entry *s=calloc(1,sizeof *s);if(!s)die("Vulkan shader allocation");strcpy(s->key,key);s->stage=meta[2];s->set=meta[3];s->count=meta[4];s->c1_size=meta[5];s->bindings=(void*)(meta+7);s->c1=(uint8_t*)s->bindings+s->count*40;memcpy(s->attributes,s->c1+s->c1_size,128);
    { unsigned k=0;shader_binding tail[256];unsigned nt=0;   /* the global-array textures (kind 5) to the end, out of the layout's bindings */
      for(unsigned i=0;i<s->count;i++){if(s->bindings[i].kind==5)tail[nt++]=s->bindings[i];else s->bindings[k++]=s->bindings[i];}
      memcpy(s->bindings+k,tail,nt*sizeof *tail);s->count=k;s->bindless=s->bindings+k;s->bindless_count=nt; }
    if(s->stage==1){size_t mb;void *marker=read_shader_file_opt(key,"early",&mb);   /* a fragment shader earlier seen with early tests */
        if(marker){free(marker);size_t pw;uint32_t *patched=spirv_add_early_tests(code,bytes/4,&pw);if(patched){free(code);code=patched;bytes=pw*4;}s->early_tests=1;}}
    if(gpl_state>0&&s->stage==1)spirv_own_set(code,bytes/4,s->set);
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes,.pCode=code};VK_CHECK(vkCreateShaderModule(device,&mi,&spv_callbacks,&s->module));
    { static int on=-1;if(on<0){const char *x=getenv("TOTK_VK_SPVID");on=x&&atoi(x);}   /* the module's identity for a driver-side list (TOTK_FP16_SKIP) */
      if(on){const uint8_t *b=(const uint8_t*)code;uint64_t h=0xcbf29ce484222325ull;for(size_t i=0;i<bytes;i++){h^=b[i];h*=0x100000001b3ull;}
        LOG("[vulkan-spvid] %s stage %d fnv %016llx",key,s->stage,(unsigned long long)h);} }
    if(s->stage==0||s->stage==1)s->io_mask=spirv_io_scan(code,bytes/4,s->stage==0?SPV_STORAGE_OUTPUT:SPV_STORAGE_INPUT,s->io);
    if(s->stage==1){spirv_io outs[32];s->frag_out_mask=spirv_io_scan(code,bytes/4,SPV_STORAGE_OUTPUT,outs);}   /* render targets the shader writes */
    spirv_uniform_extents(s,code,bytes/4);
    { static unsigned logged;if(logged<40)for(unsigned i=0;i<s->count;i++)if(s->ubo_bytes&&s->bindings[i].kind!=0&&s->bindings[i].slot>=3&&s->bindings[i].slot<19&&logged++<40)LOG("[ubo-extent] %s binding %u slot %u: %u bytes",key,s->bindings[i].binding,s->bindings[i].slot,s->ubo_bytes[i]); }
    if(s->stage==0){s->spirv=spv_keep(code,bytes);s->spirv_words=bytes/4;}else free(code);
    // 反射与 shader 同寿命；只预计算布局和索引，不缓存动态资源地址或工作区指针。
    // Reflection lives with the shader; precompute only layout and indices, never dynamic resource addresses or scratch pointers.
    if(s->count){s->binding_plan=calloc(s->count,sizeof *s->binding_plan);if(!s->binding_plan)die("Vulkan shader binding plan allocation");}
    VkDescriptorSetLayoutBinding bindings[256];for(unsigned i=0;i<s->count;i++){
        const shader_binding *b=&s->bindings[i];if(!b->count||b->count>128)die("Vulkan descriptor array capacity");
        shader_binding_plan *p=&s->binding_plan[i];p->type=descriptor_type(b);p->dynamic_index=-1;
        if(descriptor_type_uniform(p->type)||p->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER){p->offset=s->buffer_count;s->buffer_count+=b->count;}
        else if(p->type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER||p->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER){p->offset=s->texel_count;s->texel_count+=b->count;}
        else{p->offset=s->image_count;s->image_count+=b->count;}
        if(p->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER){
            unsigned base=s->stage==0?0x110:s->stage==5?0x310:0x510;int off=b->cb_offset*4;
            p->uniform_pointer=s->stage==5&&off>=0x210&&off<0x310;p->storage_slot=(off-(p->uniform_pointer?0x210:(int)base))/16;p->cb_pointer=-1;
            if(b->cb_slot>=3&&b->cb_slot<19&&off>=0){p->cb_pointer=b->cb_slot-3;p->cb_pointer_offset=off;p->storage_slot=0;p->uniform_pointer=0;}
            else if(b->cb_slot!=0||p->storage_slot<0||p->storage_slot>=16)die("Vulkan storage buffer mapping pending: shader=%s cb=%u offset=%#x",s->key,b->cb_slot,off);
        }
        bindings[i]=(VkDescriptorSetLayoutBinding){.binding=b->binding,.descriptorType=p->type,.descriptorCount=b->count,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT};
    }
    // 动态 uniform 的条数必须落在设备上限内；超限就整批退回静态，不能带病创建布局。
    // The dynamic uniform count must fit the device limit; if it does not, fall back to static for all of them
    // instead of creating a layout the device would reject.
    unsigned dynamic_limit=(unsigned)gpu_limits.maxDescriptorSetUniformBuffersDynamic,guest_candidates=0,driver_candidates=0;
    for(unsigned i=0;i<s->count;i++)if(s->binding_plan[i].type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC){
        if(descriptor_uniform_dynamic_guest(&s->bindings[i]))guest_candidates+=s->bindings[i].count;else driver_candidates+=s->bindings[i].count;}
    // 两级回退：先退掉位置相对稳定的驱动表槽位，仍超限才整批退回静态。
    // Two-tier fallback: drop the steadier driver-table slots first, and fall back to all-static only if still over.
    if(guest_candidates+driver_candidates>dynamic_limit){
        LOG("[vulkan] dynamic uniform fallback: shader=%s guest=%u driver=%u limit=%u",s->key,guest_candidates,driver_candidates,dynamic_limit);
        for(unsigned i=0;i<s->count;i++)if(s->binding_plan[i].type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC&&!descriptor_uniform_dynamic_guest(&s->bindings[i])){s->binding_plan[i].type=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;}
        if(guest_candidates>dynamic_limit)for(unsigned i=0;i<s->count;i++)if(s->binding_plan[i].type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC){s->binding_plan[i].type=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;}
    }
    /* With pipeline libraries (gpl_state) the fragment shader's set follows a set its library does not know, so the start
       of its dynamic offsets is known only at link time; Turnip gets it wrong in fast-linked pipelines on both the Odin's
       a8xx and the Thor's a740 (the fragment stage read wrong offsets: a blown-out picture, 2026-11-03). Only the vertex
       shader's set, the first, keeps dynamic uniform buffers (its start is 0 at compile time on every driver); fragment
       shaders bind theirs statically, so no driver has to patch an offset at link time. Cost (Kakariko, Odin): ~1.3 ms
       of translation-thread CPU a frame (more descriptor writes), fps unchanged. TOTK_VK_GPL_FS_DYNAMIC=1 keeps them (to
       test a driver's fix). */
    { static int keep=-1;if(keep<0){const char *e=getenv("TOTK_VK_GPL_FS_DYNAMIC");keep=e&&atoi(e);}
      if(gpl_state>0&&s->stage==1&&!keep)for(unsigned i=0;i<s->count;i++)if(s->binding_plan[i].type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC){s->binding_plan[i].type=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;} }
    // 动态偏移按 binding 号升序排位：Vulkan 要求这套数组的顺序与绑定的 binding 号一致，反射顺序不保证。
    // Dynamic offsets are ranked by ascending binding number: Vulkan requires that array order to follow binding
    // numbers, which reflection order does not guarantee.
    for(unsigned i=0;i<s->count;i++){
        unsigned best=s->count;
        for(unsigned j=0;j<s->count;j++)if(s->binding_plan[j].dynamic_index<0&&s->binding_plan[j].type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
            if(best==s->count||s->bindings[j].binding<s->bindings[best].binding)best=j;
        if(best==s->count)break;
        s->binding_plan[best].dynamic_index=(int)s->dynamic_count;s->dynamic_count+=s->bindings[best].count;
    }
    {unsigned dynamic_sum=0;
     for(unsigned i=0;i<s->count;i++)if(s->binding_plan[i].type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)dynamic_sum+=s->bindings[i].count;
     if(s->dynamic_count!=dynamic_sum)die("Vulkan dynamic uniform placement: shader=%s placed=%u expected=%u",s->key,s->dynamic_count,dynamic_sum);}
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=s->count,.pBindings=bindings};VK_CHECK(vkCreateDescriptorSetLayout(device,&li,NULL,&s->layout));
    { uint64_t h=s->count; for(unsigned i=0;i<s->count;i++){uint32_t f[4]={bindings[i].binding,(uint32_t)bindings[i].descriptorType,bindings[i].descriptorCount,bindings[i].stageFlags};for(unsigned j=0;j<4;j++){h^=f[j];h*=0x100000001b3ull;h^=h>>29;}} s->layout_hash=h|1; }
    pthread_mutex_lock(&graphics_object_lock);unsigned hb=shader_key_bucket(s->key);
    for(shader_entry *o=shader_hash[hb];o;o=o->hash_next)if(!strcmp(o->key,key)){pthread_mutex_unlock(&graphics_object_lock);
        vkDestroyDescriptorSetLayout(device,s->layout,NULL);vkDestroyShaderModule(device,s->module,&spv_callbacks);spv_free(s->spirv);free(s->binding_plan);free(s->ubo_bytes);free(meta);free(s);return o;}
    if(shader_load_private){pthread_mutex_unlock(&graphics_object_lock);shader_load_was_private=1;return s;}
    s->next=shader_list;shader_list=s;s->hash_next=shader_hash[hb];shader_hash[hb]=s;
    pthread_mutex_unlock(&graphics_object_lock);
    if(stats)resource_create_end(0,creation,s->key,NULL);LOG("[vulkan] shader %s stage=%u bindings=%u dynamic_uniforms=%u variant=%s",s->key,s->stage,s->count,s->dynamic_count,variant?shader_variant.id:"base");return s;
}
static void shader_entry_free_private(shader_entry *s) {   /* only an entry shader_load_by_key built privately: nothing else holds it */
    vkDestroyDescriptorSetLayout(device,s->layout,NULL);vkDestroyShaderModule(device,s->module,&spv_callbacks);
    for(struct linked_module *m=s->linked;m;){struct linked_module *n=m->next;vkDestroyShaderModule(device,m->module,&spv_callbacks);free(m);m=n;}
    spv_free(s->spirv);free(s->binding_plan);free(s->ubo_bytes);free((uint32_t*)(void*)s->bindings-7);free(s);
}
// TOTK: shader_get / program_shader / program_remove are in nvn_vulkan_totk_shader.inc (runtime variants); declared here,
// defined after descriptor_texture, which their answers need.
static shader_entry *program_shader(const prog_t *program,unsigned index);
static void program_remove(const prog_t *program);
static VkSamplerAddressMode wrap_mode(unsigned wrap) {switch(wrap){case 1:return VK_SAMPLER_ADDRESS_MODE_REPEAT;case 6:return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;case 4:case 5:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;default:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;}}
#include "nvn_vulkan_sampler.inc"

static texture_descriptor descriptor_texture(int id) {   /* lock-free: the table never moves; a copy that raced a write (odd or changed sequence) is retried (nvn_vulkan_tex.inc) */
    texture_descriptor d={0};if(id<0||id>=texture_capacity||!texture_descriptors)return d;
    for(;;){uint32_t seq=atomic_load_explicit(&texture_seq[id],memory_order_acquire);
        if(seq&1){__asm__ volatile("yield");continue;}
        d=texture_descriptors[id];atomic_thread_fence(memory_order_acquire);
        if(atomic_load_explicit(&texture_seq[id],memory_order_relaxed)==seq)return d;}
}
#include "nvn_vulkan_texprobe.inc"
#include "nvn_vulkan_totk_shader.inc"
#include "nvn_vulkan_shader_setup.inc"
static const shader_entry *diag_fs;   /* the drawing fragment shader, for diagnostics */
/* Attachment feedback loops (fbloop_ok, VK_EXT_attachment_feedback_loop_layout). TOTK's decals sample a G-buffer target
   they also render to (~170 one-draw passes a frame): each used to end the pass and wait on a full barrier. Now a draw
   whose fragment shader samples one of its own single-level colour targets keeps the pass: those images are listed here
   instead of going through image_access, the pipeline is built with the feedback-loop flag (Turnip then flushes per
   overlapping primitive, as UBWC needs), and draw_graphics_inner puts a by-region barrier in the pass before the draw if
   the target was written since the last one (or, starting a pass, folds the shader read into the barrier before it).
   Images stay in the GENERAL layout, in which core Vulkan allows feedback loops behind such barriers. */
static int fb_in_draw;             /* binding a graphics draw's shaders */
static image_entry *fb_images[8]; static unsigned fb_count, fb_slots;   /* fb_slots: their colour target slots */
static uint64_t fb_draws, fb_barriers;   /* feedback draws kept in their pass; in-pass barriers recorded for them */
static int fb_draw_target(const texture_descriptor *d,image_entry *e) {
    if(d->texture.d.levels>1||d->texture.L.layers>1||!(e->aspect&VK_IMAGE_ASPECT_COLOR_BIT))return 0;
    for(int i=0;i<state.rt.n;i++){const tex_t *t=state.rt.color[i];if(!t||t->mem!=d->texture.mem)continue;
        if(state.rt.has_cview[i]&&((state.rt.cview[i].set_levels&&state.rt.cview[i].base_level)||(state.rt.cview[i].set_layers&&state.rt.cview[i].min_layer)))continue;
        if(image_get(t)==e)return i+1;}
    return 0;
}
/* The per-draw part of binding a resolved texture: feedback detection, upload, hazards. Split from the resolution so a
   draw that binds the same handle to the same image as the last one (bind_shader's memo) skips the lookups. */
static void descriptor_image_sync(const texture_descriptor *d,image_entry *e,uint32_t type,int storage,VkPipelineStageFlags stage,VkAccessFlags access);
static image_entry *descriptor_image(const texture_descriptor *d,uint32_t type,int storage,VkPipelineStageFlags stage,VkAccessFlags access) {
    if(d->valid){image_entry *e=image_get(&d->texture);descriptor_image_sync(d,e,type,storage,stage,access);return e;}
    // 未绑定采样资源保留零贴图；可写空槽用独立图像，防止计算输出污染后续采样。
    // Keep unbound sampled resources zero; separate writable images prevent compute output from polluting later samples.
    static tex_t dummy[2][32];unsigned index=(type&255)+(!!(type&256))*6+(!!(type&1024))*12;if(index>=32)die("Dummy texture type %u",type);
    tex_t *t=&dummy[!!storage][index];
    if(!t->serial){t->serial=UINT64_MAX-index-(!!storage)*32;t->d=(nvn_texdesc){.target=(type&255)==1?((type&256)?NVN_TEXTURE_TARGET_1D_ARRAY:NVN_TEXTURE_TARGET_1D):(type&255)==4?NVN_TEXTURE_TARGET_3D:(type&255)==5?NVN_TEXTURE_TARGET_CUBEMAP:(type&256)?NVN_TEXTURE_TARGET_2D_ARRAY:NVN_TEXTURE_TARGET_2D,.width=1,.height=1,.depth=(type&255)==4?1:1,.levels=1,.format=(type&1024)?0x34:0x6d};t->L.layers=(type&255)==5?6:1;t->L.size=64;}
    image_entry *e=image_get(t);if(!e->populated){image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);VkClearColorValue zero={0};VkImageSubresourceRange r={e->aspect,0,1,0,e->texture.L.layers};if(type&1024){VkClearDepthStencilValue depth={1,0};vkCmdClearDepthStencilImage(command,e->image,VK_IMAGE_LAYOUT_GENERAL,&depth,1,&r);}else vkCmdClearColorImage(command,e->image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&r);e->populated=1;}
    image_access(e,stage,access);return e;
}
static void descriptor_image_sync(const texture_descriptor *d,image_entry *e,uint32_t type,int storage,VkPipelineStageFlags stage,VkAccessFlags access) {
    {   int overlapped=!storage&&overlap_check_read(e),slot;   /* TOTK_VK_OVERLAP */
        { static int diag=-1;if(diag<0){const char *x=getenv("TOTK_VK_PASSEND");diag=x&&atoi(x);}   /* sampled while an attachment of the pass: what exactly */
          static unsigned shown;if(diag&&presents>5000&&shown<40&&graphics_image_is_attachment(e)){shown++;int ai=-1;for(int i=0;i<state.rt.n;i++)if(state.rt.color[i]&&state.rt.color[i]->mem==d->texture.mem)ai=i;
              const view_t *cv=ai>=0&&state.rt.has_cview[ai]?&state.rt.cview[ai]:NULL;
              LOG("[vulkan-feedback] %ux%u fmt %#x levels %u layers %u: sampled view lv %d+%d ly %d+%d (set %d/%d), rt %d view lv %d+%d ly %d+%d; fs %s out %#x mask %#x",
                  d->texture.d.width,d->texture.d.height,d->texture.d.format,d->texture.d.levels,d->texture.L.layers,
                  d->view.set_levels?d->view.base_level:-1,d->view.set_levels?d->view.nlevels:-1,d->view.set_layers?d->view.min_layer:-1,d->view.set_layers?d->view.nlayers:-1,d->view.set_levels,d->view.set_layers,
                  ai,cv&&cv->set_levels?cv->base_level:-1,cv&&cv->set_levels?cv->nlevels:-1,cv&&cv->set_layers?cv->min_layer:-1,cv&&cv->set_layers?cv->nlayers:-1,
                  diag_fs?diag_fs->key:"-",diag_fs?diag_fs->frag_out_mask:0,(unsigned)state.mask.mask);} }
        if(fbloop_ok&&fb_in_draw&&!storage&&stage==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT&&!(access&~VK_ACCESS_SHADER_READ_BIT)&&fb_count<8&&(slot=fb_draw_target(d,e))){
            fb_slots|=1u<<(slot-1);image_upload(e);unsigned k=0;while(k<fb_count&&fb_images[k]!=e)k++;if(k==fb_count){fb_images[fb_count++]=e;synccheck_read(e,1);}
            e->last_stage|=stage;e->last_access|=access;return;}   /* a feedback loop: synchronized by draw_graphics_inner */
        if(!storage||(e->texture.has_layout&&e->texture.mem))image_upload(e);if(storage)image_gpu_written(e);if(overlapped)overlap_poison(e);image_access(e,stage,access);}
}
/* TOTK: a slot keeps whatever texture was bound last, and a shader path that does not read it can still declare it
   with another dimensionality. A view of the wrong type is invalid in Vulkan (a cube view of a 3D image), so such a
   texture is replaced by the stand-in of the declared type. */
static int descriptor_type_fits(const tex_t *t,uint32_t type) {
    int target=t->d.target,cube=target==NVN_TEXTURE_TARGET_CUBEMAP||target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY,is3d=target==NVN_TEXTURE_TARGET_3D;
    switch(type&255){
    case 4: return is3d;
    case 5: return cube&&t->L.layers>=6&&!(t->L.layers%6);
    case 1: return target==NVN_TEXTURE_TARGET_1D||target==NVN_TEXTURE_TARGET_1D_ARRAY;
    default: return !is3d&&target!=NVN_TEXTURE_TARGET_1D&&target!=NVN_TEXTURE_TARGET_1D_ARRAY;
    }
}
static cached_image_view *descriptor_view_entry(const texture_descriptor *descriptor,image_entry *e,uint32_t type,int storage) {
    view_t v=descriptor->valid?descriptor->view:(view_t){0};
    unsigned mip=v.set_levels?v.base_level:0,layers=v.set_layers?v.min_layer:0;
    if(mip>=e->texture.d.levels||layers>=e->texture.L.layers)die("Vulkan descriptor view bounds");
    VkImageViewType vt;switch(type&255){case 1:vt=(type&256)?VK_IMAGE_VIEW_TYPE_1D_ARRAY:VK_IMAGE_VIEW_TYPE_1D;break;case 4:vt=VK_IMAGE_VIEW_TYPE_3D;break;case 5:vt=(type&256)?VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:VK_IMAGE_VIEW_TYPE_CUBE;break;default:vt=(type&256)?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;}
    uint32_t levels=storage?1:v.set_levels&&v.nlevels>0?v.nlevels:e->texture.d.levels-mip,count=v.set_layers&&v.nlayers>0?v.nlayers:e->texture.L.layers-layers;
    if(vt==VK_IMAGE_VIEW_TYPE_1D||vt==VK_IMAGE_VIEW_TYPE_2D||vt==VK_IMAGE_VIEW_TYPE_3D)count=1;
    VkImageAspectFlags aspect=e->aspect & VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:e->aspect;
    VkComponentMapping components=storage||!descriptor->valid?(VkComponentMapping){0}:texture_components(&descriptor->texture,&v);   /* storage views must be identity */
    /* the texture object's own format (a storage-shared image may belong to another format of the same size) */
    VkFormat format=descriptor->valid?vk_format(v.set_format?v.format:descriptor->texture.d.format):0;
    return image_view_formatted(e,vt,(VkImageSubresourceRange){aspect,mip,levels,layers,count},components,format);
}
static VkImageView descriptor_view(const texture_descriptor *descriptor,image_entry *e,uint32_t type,int storage) {return descriptor_view_entry(descriptor,e,type,storage)->view;}
#include "nvn_vulkan_uniform.inc"
static VkPipelineStageFlags shader_pipeline_stage(const shader_entry *s);
#include "nvn_vulkan_bindless.inc"
static uint64_t uniform_trimmed_bytes;   /* uniform bytes not read because the shader cannot reach them */
static int uniform_compose_on(void);static VkDescriptorBufferInfo uniform_compose(uintptr_t address,uint32_t bytes);
static int forward_compose_possible(uintptr_t address,uint64_t size,post_op *const *scope_marker);
#include "nvn_vulkan_binding_cache.inc"
static int bindstats_on(void);static void bindstats_capture(const shader_entry *s);   /* nvn_vulkan_bindstats.inc */
static int taa_enabled(void);   /* nvn_vulkan_taa.inc: its jitter edits our copies of the camera blocks */
/* TOTK: a storage buffer whose address the shader reads from one of its own constant buffers (c3..c18, at cb_pointer_offset)
   instead of the driver's c0 table: the 16-byte record there, {address, size}, as the game bound it at submit (first met
   2026-10-23 in vertex shader ba632d0ac006185c, c6 offset 0). An address outside the pools binds nothing (zeros).
   The Depths' terrain (vertex shaders fa2a1f8e, 7a4eee06...) pulls its vertices through such a record, set by an inline
   update before each draw, so it is read as uniforms are: the submit call's bytes with the pending updates over them.
   Its second word is no size (about 270 MB, past the end of the pool, and no shader reads it), so a good address gets the
   rest of its pool. Until 2026-11-04 those records bound zeros: every vertex of the ground fell on one point, and the
   backdrop far below showed through as black ground. */
static uint64_t mtl_room(uint64_t addr);
static void storage_pointer_resolve(unsigned stage,int ubo,int offset,p_bind_buf *out) {
    *out=(p_bind_buf){0};const p_bind_buf *u=&state.ubo[stage][ubo];
    if(!u->addr||offset<0||(uint64_t)offset+16>(u->size?u->size:65536))return;
    if(uniform_read_current((uintptr_t)(u->addr+offset),16))return;
    uint64_t address;uint32_t size;memcpy(&address,uniform_read_scratch,8);memcpy(&size,uniform_read_scratch+8,4);
    if(!address||!size)return;
    uint64_t bytes=size;
    if(!mtl_addr(address,bytes,NULL,NULL)){bytes=mtl_room(address);if(bytes>gpu_limits.maxStorageBufferRange)bytes=gpu_limits.maxStorageBufferRange;}
    if(bytes){out->addr=address;out->size=bytes;}
}
// arena 槽位承载的 uniform 把位置交给动态偏移；描述符只留块基址与范围，同一块内换槽位就不再改变描述符。
// A uniform carried by an arena slot hands its position to the dynamic offset; the descriptor keeps only the block
// base and the range, so moving slots inside one block no longer changes the descriptor.
// The shader's plan decides, not descriptor_uniform_dynamic: the layout may have made a candidate static (the device
// limit, pipeline libraries), and a static descriptor must keep its offset.
static VkDescriptorBufferInfo uniform_dynamic_split(const shader_entry *s,const shader_binding *b,VkDescriptorBufferInfo info,uint32_t *dynamic_offset) {
    if(s->binding_plan[b-s->bindings].type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)return info;
    if(info.offset>(uint64_t)UINT32_MAX)die("Vulkan dynamic uniform offset range");
    *dynamic_offset=(uint32_t)info.offset;info.offset=0;return info;
}
static uint64_t uniform_forwarded;
static VkPipelineStageFlags shader_pipeline_stage(const shader_entry *s);
static VkDescriptorBufferInfo forward_readonly_buffer(uintptr_t address,uint64_t bytes,uint64_t alignment,VkPipelineStageFlags stage,VkAccessFlags access);
static int dependencies_unknown(uintptr_t address,uint64_t size);
static VkDescriptorBufferInfo uniform_buffer(shader_entry *s,const shader_binding *b,uint32_t *dynamic_offset) {
    *dynamic_offset=0;
    // 支持缓冲与每个着色器的 c1 始终不变，跨绘制共享，避免反复清零 64 KiB。
    // Support data and each shader's c1 are immutable; share across draws instead of repeatedly clearing 64 KiB.
    static vk_buffer support,zero;
    if(b->kind==0){if(!support.buffer){support=buffer_create(65536);memset(support.mapped,0,65536);for(int i=0;i<73;i++)*(float*)((uint8_t*)support.mapped+192+16*i)=1;immutable_buffer_bytes+=support.capacity;}return (VkDescriptorBufferInfo){support.buffer,0,65536};}
    if(b->slot==1){if(!s->constant_buffer.buffer){s->constant_buffer=buffer_create(65536);memset(s->constant_buffer.mapped,0,65536);memcpy(s->constant_buffer.mapped,s->c1,s->c1_size);immutable_buffer_bytes+=s->constant_buffer.capacity;}return (VkDescriptorBufferInfo){s->constant_buffer.buffer,0,65536};}
    if(b->slot==0){VkDescriptorBufferInfo current;if(uniform_driver_current(s->stage,0,&current))return uniform_dynamic_split(s,b,current,dynamic_offset);
        /* TOTK: c0 0x610 holds the point-size limit; the grass-removal stamps (vertex shaders 494f391c0abd9026 and three
           others: one point per object footprint into the 256x256 grass mask) write PointSize = min(diameter, c0[0x610].x).
           Past the end of the table it read 0, every stamp had size 0 and grass grew through floors (2026-10-23). The
           limit is the device's (Maxwell 2047.94 at most). */
        uint8_t data[0x620]={0};unsigned base=s->stage==0?0x110:s->stage==5?0x310:0x510;
        { float limit=gpu_limits.pointSizeRange[1]<2047.9375f?gpu_limits.pointSizeRange[1]:2047.9375f;memcpy(data+0x610,&limit,4); }
        for(unsigned i=0;i<16;i++){memcpy(data+base+16*i,&state.ssbo[s->stage][i].addr,8);uint32_t size=state.ssbo[s->stage][i].size;memcpy(data+base+16*i+8,&size,4);}
        // 计算 c0 同时保存 0x210 的 UBO 地址表和 0x310 的 SSBO 地址表。
        // Compute c0 contains both the UBO address table at 0x210 and the SSBO table at 0x310.
        if(s->stage==5)for(unsigned i=0;i<16;i++){memcpy(data+0x210+16*i,&state.ubo[5][i].addr,8);uint32_t size=state.ubo[5][i].size;memcpy(data+0x210+16*i+8,&size,4);}
        return uniform_dynamic_split(s,b,uniform_driver(s->stage,0,data,sizeof data),dynamic_offset);}
    if(b->slot==2){VkDescriptorBufferInfo current;if(uniform_driver_current(s->stage,1,&current))return uniform_dynamic_split(s,b,current,dynamic_offset);
        uint8_t data[0x968]={0};for(unsigned i=0;i<128;i++){uint32_t tex=state.textures[s->stage][i],samp=state.samplers[s->stage][i];memcpy(data+0x168+8*i,&tex,4);memcpy(data+0x568+8*i,&samp,4);}for(unsigned i=0;i<8;i++){uint32_t image=state.images[s->stage][i];memcpy(data+0x120+8*i,&image,4);}
        for(unsigned i=0;i<32;i++){uint32_t tex=state.textures[s->stage][i];memcpy(data+0x20+8*i,&tex,4);}   /* TOTK combined handles */
        return uniform_dynamic_split(s,b,uniform_driver(s->stage,1,data,sizeof data),dynamic_offset);}
    if(b->slot==BL_SLOT_BUFFER&&bl_set&&s->stage<6){uint32_t words=bl_block_words[s->stage]>4?bl_block_words[s->stage]:4;   /* bindless: the slot block (nvn_vulkan_bindless.inc) */
        return uniform_dynamic_split(s,b,uniform_driver(s->stage,2,(const uint8_t*)bl_block[s->stage],words*4),dynamic_offset);}
    if(b->slot>=3&&b->slot<19){const p_bind_buf *u=&state.ubo[s->stage][b->slot-3];uint32_t n=u->addr&&u->size?(u->size<65536?u->size:65536):0;
        { uint32_t used=s->ubo_bytes?s->ubo_bytes[b-s->bindings]:0;if(used&&n>used){uniform_trimmed_bytes+=n-used;n=used;} }   /* only what the shader reads */
        // 未绑定 uniform 的完整范围同为零，可共享一个不可变页，不占每 draw 的 arena 空间。
        // Unbound uniforms have the same all-zero range, so share an immutable page without per-draw arena space.
        if(!n){if(!zero.buffer){zero=buffer_create(65536);memset(zero.mapped,0,65536);immutable_buffer_bytes+=zero.capacity;}return (VkDescriptorBufferInfo){zero.buffer,0,65536};}
        /* the capture first: a clean block binds without a pending-write query; a dirty one (a GPU write pending over it)
           falls through to the compose check, as before */
        { uint32_t used=s->ubo_bytes?s->ubo_bytes[b-s->bindings]:0;VkDescriptorBufferInfo info;
          /* the shader reads only bytes the game bound: past them the arena gives zeros. A shader whose reach is unknown
             (dynamic indexing) qualifies when the game bound all 64 KiB. */
          if(!used&&u->size>=65536)used=65536;
          /* With robustBufferAccess2 the descriptor's exact range gives zeros past the bound bytes, as the arena does (and as
             the console's constant buffer binding does), so a shader whose reach is unknown, or reads past the bound size,
             can bind the capture at that range. Without it only a shader known to stay inside the bound bytes can. */
          if(inplace_on()&&inplace_ubo_on&&ubo_robust_ok){uint64_t off;uint32_t range=(n+15)&~15u;VkBuffer pool=inplace_kept(u->addr)?VK_NULL_HANDLE:zc_buffer_at(u->addr,range,&off);
              if(pool&&!(gpu_limits.minUniformBufferOffsetAlignment&&off%gpu_limits.minUniformBufferOffsetAlignment)){if(!s->umemo){s->umemo=calloc(s->count?s->count:1,sizeof *s->umemo);if(!s->umemo)die("Vulkan uniform memo");}
                  if(ucap_clean(u->addr,n,&s->umemo[b-s->bindings])){inplace_ubo++;if(inplace_state==2)inplace_check(u->addr,n,&s->umemo[b-s->bindings]);return uniform_dynamic_split(s,b,(VkDescriptorBufferInfo){pool,off,range},dynamic_offset);}inplace_why[IF_UBO_DIRTY]++;}
              else{inplace_why[inplace_kept(u->addr)?IF_UBO_KEPT:pool?IF_UBO_ALIGN:IF_UBO_POOL]++;if(!pool&&!inplace_kept(u->addr))inplace_classify(u->addr,range);}
              inplace_fallback++;}
          if(ucap_bind_on()){int exact=ubo_robust_ok&&(!used||used>u->size);
              if(!exact&&!used)ucap_miss(UCAP_EXTENT_UNKNOWN,n);else if(!exact&&used>u->size)ucap_miss(UCAP_READS_PAST,n);else if(taa_enabled()>0)ucap_miss(UCAP_TAA,n);
              else{if(!s->umemo){s->umemo=calloc(s->count?s->count:1,sizeof *s->umemo);if(!s->umemo)die("Vulkan uniform memo");}
                  if(ucap_bind(u->addr,n,exact?(n+15)&~15u:65536,&info,&s->umemo[b-s->bindings]))return uniform_dynamic_split(s,b,info,dynamic_offset);}} }
        if(uniform_compose_on()&&!(n&3)&&async_unknown(u->addr,n)&&forward_compose_possible(u->addr,n,forward_scope_marker))return uniform_dynamic_split(s,b,uniform_compose(u->addr,n),dynamic_offset);
        return uniform_dynamic_split(s,b,uniform_read_cached(u->addr,n),dynamic_offset);}
    die("Unsupported Vulkan constant slot %u",b->slot);
}
typedef struct descriptor_chunk { VkDescriptorPool pool; struct descriptor_chunk *next; } descriptor_chunk;
static descriptor_chunk *descriptor_chunks,*descriptor_current;
static void descriptor_reset(void) {
    // 前一批 GPU 已完成，可整体重用描述符；容量不足时保留额外块供后续批次使用。
    // The previous batch has completed, so descriptors can be reset together; retain extra chunks for later batches.
    for(descriptor_chunk *c=descriptor_chunks;c;c=c->next)VK_CHECK(vkResetDescriptorPool(device,c->pool,0));descriptor_current=descriptor_chunks;uniform_reset();binding_cache_reset();
}
static descriptor_chunk *descriptor_chunk_new(void) {
    descriptor_chunk *c=calloc(1,sizeof *c);if(!c)die("Vulkan descriptor chunk allocation");
    // 池必须覆盖用到的每个描述符类型；动态 uniform 是类型 8，只列 0-7 会让分配失败。
    // The pool must cover every descriptor type in use; dynamic uniform is type 8, so listing only 0-7 makes allocation fail.
    VkDescriptorPoolSize sizes[10];for(unsigned i=0;i<10;i++)sizes[i]=(VkDescriptorPoolSize){i,4096};
    VkDescriptorPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=256,.poolSizeCount=10,.pPoolSizes=sizes};VK_CHECK(vkCreateDescriptorPool(device,&pi,NULL,&c->pool));return c;
}
static VkDescriptorSet descriptor_allocate(VkDescriptorSetLayout layout) {
    if(!descriptor_current){descriptor_chunks=descriptor_chunk_new();descriptor_current=descriptor_chunks;}
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorSetCount=1,.pSetLayouts=&layout};VkDescriptorSet set;
    for(;;){ai.descriptorPool=descriptor_current->pool;VkResult result=vkAllocateDescriptorSets(device,&ai,&set);if(result==VK_SUCCESS)return set;
        if(result!=VK_ERROR_OUT_OF_POOL_MEMORY&&result!=VK_ERROR_FRAGMENTED_POOL)die("Vulkan descriptor allocation: %d",result);
        if(!descriptor_current->next){descriptor_current->next=descriptor_chunk_new();ai.descriptorPool=descriptor_current->next->pool;VK_CHECK(vkAllocateDescriptorSets(device,&ai,&set));descriptor_current=descriptor_current->next;return set;}
        descriptor_current=descriptor_current->next;
    }
}
typedef struct descriptor_scratch {
    VkDescriptorBufferInfo *buffers;
    VkDescriptorImageInfo *images;
    VkBufferView *texels;
    size_t buffer_capacity,image_capacity,texel_capacity;
} descriptor_scratch;
static descriptor_scratch descriptor_workspace;
static void descriptor_reserve(shader_entry *s) {
    size_t buffers=s->buffer_count,images=s->image_count,texels=s->texel_count;
    // 队列锁串行调用 bind_shader；保留 CPU 工作区，所有 binding 一次更新，避免逐项 Vulkan 调用。
    // The queue lock serializes bind_shader; retain CPU scratch and update all bindings with one Vulkan call.
    if(buffers>descriptor_workspace.buffer_capacity){descriptor_workspace.buffers=realloc(descriptor_workspace.buffers,buffers*sizeof *descriptor_workspace.buffers);if(!descriptor_workspace.buffers)die("Vulkan descriptor buffers allocation");descriptor_workspace.buffer_capacity=buffers;}
    if(images>descriptor_workspace.image_capacity){descriptor_workspace.images=realloc(descriptor_workspace.images,images*sizeof *descriptor_workspace.images);if(!descriptor_workspace.images)die("Vulkan descriptor images allocation");descriptor_workspace.image_capacity=images;}
    if(texels>descriptor_workspace.texel_capacity){descriptor_workspace.texels=realloc(descriptor_workspace.texels,texels*sizeof *descriptor_workspace.texels);if(!descriptor_workspace.texels)die("Vulkan descriptor texels allocation");descriptor_workspace.texel_capacity=texels;}
}
static VkPipelineStageFlags shader_pipeline_stage(const shader_entry *s) {
    switch(s->stage){case 0:return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;case 1:return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;case 5:return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;default:die("Unsupported Vulkan shader stage %u",s->stage);}
}
#include "nvn_vulkan_forward_bind.inc"
#include "nvn_vulkan_texel.inc"
/* A storage buffer whose latest version is an earlier shader write of exactly this range in the batch being recorded is
   written in place, as the GPU would: no new version seeded with a copy of the old one (each copy ended the render pass:
   ~300 passes a frame in the open world, where one 128.5 KiB buffer is written by ~260 draws). Inside a pass the draws
   are not separated by a barrier, as the game's own barriers inside passes are not either; outside one they are. The
   batch's write-back of that version publishes the final bytes. TOTK_VK_EXP=ssbocopy: a seeded copy per write. */
static uint64_t ssbo_in_place_writes,ssbo_war_barriers,ssbo_pass_barriers;
static uint64_t ssbo_barrier_epoch;   /* game barriers inside passes (OP_BARRIER skips them; storage writes in place still order on them) */
/* The current draw's in-place writes (its bindings come before it knows its render pass): skipped[] went without a
   barrier because the version's latest write is in the pass open at binding, which holds only if the draw stays in it
   (ssbo_in_place_before_pass). */
static struct { post_op *written[8],*skipped[8]; VkPipelineStageFlags stage[8]; unsigned nwritten,nskipped; uint64_t pass; } ssbo_draw;
uint64_t ssbo_in_place_writes_total(void) { return ssbo_in_place_writes; }
uint64_t ssbo_war_barriers_total(void) { return ssbo_war_barriers; }
uint64_t ssbo_pass_barriers_total(void) { return ssbo_pass_barriers; }
static void ssbo_in_place_draw_begin(void) { ssbo_draw.nwritten=ssbo_draw.nskipped=0; }
static int ssbo_write_in_place(const shader_entry *s,uintptr_t address,uint64_t bytes,VkDescriptorBufferInfo *out) {
    static int off=-1;if(off<0)off=exp_env("ssbocopy");
    if(off||(bytes&3))return 0;
    forward_slice slice;if(forward_find(address,bytes,4,forward_scope_marker,&slice)!=FORWARD_FULL||slice.host_written)return 0;
    post_op *q=slice.producer;
    if(!q||q->kind!=0||q->site!=POST0_SITE_SSBO||q->batch_serial!=post_batch_serial||(uintptr_t)q->destination!=address||q->size!=bytes||slice.offset||q->seed)return 0;
    VkPipelineStageFlags stage=shader_pipeline_stage(s);
    /* write after read: reads of this version by earlier draws (a vertex shader reading what a later draw's fragment
       shader overwrites) finish first; ~5 a frame. The draw's own reads are its business, as on the GPU. */
    VkPipelineStageFlags reads=q->read_stages|(q->read_draw!=draw_count?q->read_stages_draw:0);
    if(reads){graphics_pass_end();VkMemoryBarrier m={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        vkCmdPipelineBarrier(command,reads,stage,0,1,&m,0,NULL,0,NULL);
        q->read_stages=0;if(q->read_draw!=draw_count)q->read_stages_draw=0;ssbo_war_barriers++;}
    /* Draws in one pass write without barriers between them, as the GPU does (unless the game put a barrier between
       them); the latest write from anywhere else (a compute dispatch, an earlier pass, the seed copy) is waited for,
       ending the pass if one is open (~30 a frame). Before, a write into whatever pass was open at binding skipped it,
       e.g. the sky's cloud parameters (a fragment shader reading and writing what a compute dispatch just wrote, in a
       pass opened after it). Found while chasing the cloud flicker (fixed in forward_read_barrier, not here). */
    int same_pass=stage!=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT&&graphics_pass_open()&&q->inplace_pass==render_passes&&q->inplace_epoch==ssbo_barrier_epoch;
    if(!same_pass){graphics_pass_end();VkBufferMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=q->gpu_access,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=q->output.buffer,.size=bytes};
        vkCmdPipelineBarrier(command,q->gpu_stage,stage,0,0,NULL,1,&b,0,NULL);ssbo_pass_barriers++;}
    else if(ssbo_draw.nskipped<8){ssbo_draw.skipped[ssbo_draw.nskipped]=q;ssbo_draw.stage[ssbo_draw.nskipped++]=stage;ssbo_draw.pass=render_passes;}
    else{graphics_pass_end();VkMemoryBarrier m={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
        vkCmdPipelineBarrier(command,q->gpu_stage,stage,0,1,&m,0,NULL,0,NULL);}
    if(stage==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)q->inplace_pass=0;
    else if(ssbo_draw.nwritten<8)ssbo_draw.written[ssbo_draw.nwritten++]=q;
    else q->inplace_pass=0;
    /* readers after this draw synchronize with its writes too */
    q->gpu_stage|=stage;q->gpu_access|=VK_ACCESS_SHADER_WRITE_BIT;memset(q->gpu_synchronized,0,sizeof q->gpu_synchronized);
    if(fwd_watch(address,bytes))LOG("[fwdwatch] frame %llu draw %llu in-place write %s stage %#x producer order %llu pass %s",(unsigned long long)presents,(unsigned long long)draw_count,s->key,stage,(unsigned long long)q->order,graphics_pass_open()?"open":"closed");
    ssbo_in_place_writes++;*out=(VkDescriptorBufferInfo){q->output.buffer,0,bytes};return 1;
}
static VkDescriptorSet bind_shader(shader_entry *s,post_op *lifetime,uint32_t *dynamic_offsets,unsigned *dynamic_count) {
    (void)lifetime;descriptor_reserve(s);
    if(s->bindless_count)bindless_resolve(s);   /* before the bindings: c[2] (slot 2) carries the slot table */
    unsigned dynamic_base=*dynamic_count;
    // 两个集合的动态偏移必须连续排在一套数组里：先第一个集合的全部偏移，再第二个集合的全部偏移。
    // Both sets' dynamic offsets must be contiguous in one array: every offset of the first set, then the second's.
    if(dynamic_base+s->dynamic_count>256)die("Vulkan dynamic uniform offset capacity: shader=%s base=%u count=%u",s->key,dynamic_base,s->dynamic_count);
    *dynamic_count=dynamic_base+s->dynamic_count;
    VkWriteDescriptorSet writes[256];
    uint64_t res_phase=dm_cpu_phase_begin();
    for(unsigned i=0;i<s->count;i++){
        const shader_binding *b=&s->bindings[i];const shader_binding_plan *plan=&s->binding_plan[i];VkDescriptorType type=plan->type;VkDescriptorBufferInfo *buffers=NULL;VkDescriptorImageInfo *images=NULL;VkBufferView *texel_views=NULL;
        if(descriptor_type_uniform(type)||type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)buffers=descriptor_workspace.buffers+plan->offset;
        else if(type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER||type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)texel_views=descriptor_workspace.texels+plan->offset;
        else images=descriptor_workspace.images+plan->offset;
        VkWriteDescriptorSet w={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=b->binding,.descriptorCount=b->count,.descriptorType=type,.pBufferInfo=buffers,.pImageInfo=images,.pTexelBufferView=texel_views};
        VkDescriptorBufferInfo uniform={0};uint32_t uniform_offset=0;
        if(descriptor_type_uniform(type)){uniform=uniform_buffer(s,b,&uniform_offset);if(plan->dynamic_index>=0)for(unsigned j=0;j<b->count;j++)dynamic_offsets[dynamic_base+plan->dynamic_index+j]=uniform_offset;}
        for(unsigned j=0;j<b->count;j++){
            if(descriptor_type_uniform(type))buffers[j]=uniform;
            else if(type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER){
                int uniform_ptr=plan->uniform_pointer,slot=plan->storage_slot;p_bind_buf pointed;
                const p_bind_buf *u=plan->cb_pointer>=0?(storage_pointer_resolve(s->stage,plan->cb_pointer,plan->cb_pointer_offset,&pointed),&pointed):uniform_ptr?&state.ubo[s->stage][slot]:&state.ssbo[s->stage][slot];uint64_t bytes=u->size?u->size:65536;vk_buffer buffer;
                // A storage pointer's window is the rest of its pool (up to the device limit, 146 MB seen), so it is bound in place: forwarded,
                // it left copies over the whole pool, and after the Depths other draws read old data from them as vertices (slabs over Gerudo Town).
                // BufferUsageFlags.Write 是位 0；图像 flags 的位号不可用于 SSBO。
                // BufferUsageFlags.Write is bit 0; image flag bits do not apply to SSBOs.
                if(u->addr&&!(b->flags&1)&&plan->cb_pointer<0){if(!s->bmemo){s->bmemo=calloc(s->buffer_count?s->buffer_count:1,sizeof *s->bmemo);if(!s->bmemo)die("Vulkan buffer memo");}
                    buffers[j]=forward_readonly_shader_buffer(&s->bmemo[plan->offset+j],s,b,j,slot,UINT32_MAX,uniform_ptr?2:1,u->size,u->addr,bytes,gpu_limits.minStorageBufferOffsetAlignment,shader_pipeline_stage(s),VK_ACCESS_SHADER_READ_BIT);}
                else if(u->addr&&(inplace_on()||plan->cb_pointer>=0)&&({uint64_t off_;VkBuffer pool_=zc_buffer_at(u->addr,bytes,&off_);int ok_=pool_&&!(off_%gpu_limits.minStorageBufferOffsetAlignment);if(ok_){buffers[j]=(VkDescriptorBufferInfo){pool_,off_,bytes};inplace_ssbo_w++;}else{inplace_fallback++;inplace_why[pool_?IF_SSBO_ALIGN:IF_SSBO_POOL]++;if(!pool_)inplace_classify(u->addr,bytes);}ok_;})){}
                else if(u->addr&&ssbo_write_in_place(s,u->addr,bytes,&buffers[j])){}
                else{post_op *p=post_new(u->addr?0:3);p->site=POST0_SITE_SSBO;p->destination=(void*)(uintptr_t)u->addr;p->size=bytes;p->output=buffer_create(bytes);if(u->addr){post_register_range(p);const uint8_t *inherit;int cpu=forward_seed(&p->output,u->addr,bytes,shader_pipeline_stage(s),&inherit);
                    static int full=-1;if(full<0)full=!exp_env("diffpublish");   /* experiment: publish changed blocks only (did not help the flicker) */
                    if(!full&&(cpu||inherit)&&(p->seed=malloc(bytes)))memcpy(p->seed,cpu?p->output.mapped:inherit,bytes);}else memset(p->output.mapped,0,bytes);buffer=p->output;buffers[j]=(VkDescriptorBufferInfo){buffer.buffer,0,bytes};}w.pBufferInfo=buffers;
            }
            else if(type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER||type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER){
                int writable=type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;unsigned id=shader_handle(b,s->stage,j)&0xfffff;
                texture_descriptor d=descriptor_texture(id);
                uint64_t bytes=d.valid?d.texture.L.size:65536,read_before=snapshot_bytes,forward_before=forward_read_bytes;VkDescriptorBufferInfo buffer;
                // 只读类型化缓冲也由 GPU 直接消费已编码的完整版本；可写初值保留原 CPU 等待路径。
                // Read-only typed buffers consume complete recorded versions on the GPU; writable initial contents retain the existing CPU-wait path.
                if(d.valid&&!writable)buffer=forward_readonly_shader_buffer(NULL,s,b,j,UINT32_MAX,id,3,d.texture.L.size,(uintptr_t)d.texture.mem,bytes,texel_buffer_alignment(&d),shader_pipeline_stage(s),VK_ACCESS_SHADER_READ_BIT);
                else{post_op *p=post_new(d.valid&&writable?0:3);p->site=POST0_SITE_TEXEL;p->size=bytes;p->output=buffer_create(bytes);memset(p->output.mapped,0,bytes);if(d.valid&&snapshot_read((uintptr_t)d.texture.mem,p->output.mapped,bytes))die("Vulkan texel-buffer read");if(d.valid&&writable){p->destination=d.texture.mem;post_register_range(p);}buffer=(VkDescriptorBufferInfo){p->output.buffer,0,bytes};}
                texel_cpu_read_bytes+=snapshot_bytes-read_before;texel_forward_bytes+=forward_read_bytes-forward_before;
                texel_views[j]=texel_view_get(buffer,d.valid?vk_format(d.view.set_format?d.view.format:d.texture.d.format):VK_FORMAT_R32G32B32A32_SFLOAT);w.pTexelBufferView=texel_views;
            }
            else {
                uint32_t handle=shader_handle(b,s->stage,j);
                images[j]=(VkDescriptorImageInfo){.sampler=sampler_get_for_type((handle>>20)&0xfff,type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER?b->type:0),.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
                if(type!=VK_DESCRIPTOR_TYPE_SAMPLER){
                    if(!s->tmemo){s->tmemo=calloc(s->image_count?s->image_count:1,sizeof *s->tmemo);if(!s->tmemo)die("Vulkan texture memo");}
                    struct tex_memo *m=&s->tmemo[plan->offset+j];unsigned id=handle&0xfffff;
                    uint32_t seq=id<(unsigned)texture_capacity?atomic_load_explicit(&texture_seq[id],memory_order_acquire):1;
                    VkAccessFlags access=VK_ACCESS_SHADER_READ_BIT;if(b->kind==4&&(b->flags&4))access|=VK_ACCESS_SHADER_WRITE_BIT;
                    if(m->e&&m->handle==handle&&m->seq==seq&&m->gen==image_generation){   /* the same handle to the same image as last time */
                        images[j].imageView=m->view;descriptor_image_sync(&m->d,m->e,b->type,b->kind==4,shader_pipeline_stage(s),access);
                        if(texprobe_on()&&b->kind!=4)texprobe_use(id,b->type,&m->d,m->view);w.pImageInfo=images;continue;}
                    texture_descriptor descriptor=descriptor_texture(id);
                    // 只读计数：游戏绑了句柄、端口却查不到登记，就会退回 1x1 替身贴图，整个四边形采到同一个颜色。
                    // Read-only counter: the game bound a handle the port cannot resolve, which falls back to a 1x1
                    // stand-in texture; the whole quad then samples one colour.
                    if(handle&&!descriptor.valid){gpu_unregistered_textures++;if(gpu_unregistered_textures<=10||!(gpu_unregistered_textures%256))LOG("[vulkan-unregistered-tex] 绑定了未登记的贴图 id=%u handle=%#x shader=%s slot=%d（退回 1x1 替身）",handle&0xfffff,handle,s->key,b->binding);}
                    if(descriptor.valid&&!descriptor_type_fits(&descriptor.texture,b->type)){static unsigned said;if(said++<10)LOG("[vulkan] texture id=%u target %d does not fit shader slot type %#x (shader=%s slot=%d): stand-in used",handle&0xfffff,descriptor.texture.d.target,b->type,s->key,b->binding);descriptor.valid=0;}
                    // ImageStore 是位 2；保守保留读取权限，反射未单独标注 imageLoad。
                    // ImageStore is bit 2; conservatively retain reads because reflection does not separately flag imageLoad.
                    image_entry *e=descriptor_image(&descriptor,b->type,b->kind==4,shader_pipeline_stage(s),access);images[j].imageView=descriptor_view(&descriptor,e,b->type,b->kind==4);
                    if(descriptor.valid&&!(seq&1)&&atomic_load_explicit(&texture_seq[id],memory_order_relaxed)==seq)*m=(struct tex_memo){handle,seq,image_generation,e,images[j].imageView,descriptor};
                    else m->e=NULL;
                    if(texprobe_on()&&b->kind!=4)texprobe_use(handle&0xfffff,b->type,&descriptor,images[j].imageView);}
                w.pImageInfo=images;
            }
        }
        writes[i]=w;
    }
    dm_cpu_phase_end(DM_CPU_PHASE_RESOLVE,res_phase);
    { uintptr_t wa=gpu_watch_address();static unsigned shown;   /* TOTK_VK_WATCH_MEM: the shader writing the watched window and what it reads */
      if(wa&&shown<400)for(unsigned i=0;i<s->count;i++){const shader_binding *b=&s->bindings[i];if(s->binding_plan[i].type!=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||!(b->flags&1))continue;
          int uniform_ptr=s->binding_plan[i].uniform_pointer,slot=s->binding_plan[i].storage_slot;p_bind_buf pointed;
          const p_bind_buf *u=s->binding_plan[i].cb_pointer>=0?(storage_pointer_resolve(s->stage,s->binding_plan[i].cb_pointer,s->binding_plan[i].cb_pointer_offset,&pointed),&pointed):uniform_ptr?&state.ubo[s->stage][slot]:&state.ssbo[s->stage][slot];
          if(!(u->addr&&gpu_watch_hit(u->addr,u->size?u->size:65536)))continue;
          shown++;LOG("[vulkan-watch] frame %llu draw %llu shader %s stage %d writes %#llx+%llu",(unsigned long long)presents,(unsigned long long)draw_count,s->key,s->stage,(unsigned long long)u->addr,(unsigned long long)u->size);
          for(unsigned k=0;k<s->count;k++){const shader_binding *t=&s->bindings[k];VkDescriptorType ty=s->binding_plan[k].type;
              if(ty==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER||ty==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE||ty==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)for(unsigned j=0;j<t->count;j++){
                  uint32_t handle=shader_handle(t,s->stage,j);texture_descriptor d=descriptor_texture(handle&0xfffff);image_entry *e=d.valid?image_get(&d.texture):NULL;
                  LOG("[vulkan-watch]   binding %u[%u] handle %#x: %s fmt %#x view fmt %#x %ux%u lv%u mem %#llx+%llu serial %llu gpu_written %d (frame %llu) populated %d",t->binding,j,handle,d.valid?"valid":"INVALID",
                      d.valid?d.texture.d.format:0,d.valid&&d.view.set_format?d.view.format:0,d.valid?d.texture.d.width:0,d.valid?d.texture.d.height:0,d.valid?d.texture.d.levels:0,
                      (unsigned long long)(uintptr_t)(d.valid?d.texture.mem:NULL),(unsigned long long)(d.valid?d.texture.L.size:0),(unsigned long long)(d.valid?d.texture.serial:0),
                      e?e->gpu_written:-1,(unsigned long long)(e?e->gpu_write_frame:0),e?e->populated:-1);
                  if(e)LOG("[vulkan-watch]     entry serial %llu holders %u keyed %d",(unsigned long long)e->serial,e->holders,e->storage_keyed);}
              else if(ty==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||descriptor_type_uniform(ty))LOG("[vulkan-watch]   binding %u: %s",t->binding,ty==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER?"storage buffer":"uniform");}
      } }
    // 仍逐次执行上传/访问风险处理，再比较真实 payload；命中不能跳过 GPU 同步。
    // Perform uploads/access hazard handling on every call before comparing actual payloads; hits never skip GPU synchronization.
    graphics_probe_cs_inputs(s,writes,s->count,dynamic_offsets+dynamic_base,s->dynamic_count);
    uint64_t descset_phase=dm_cpu_phase_begin();
    if(bindstats_on())bindstats_capture(s);
    size_t nb=s->buffer_count*sizeof *descriptor_workspace.buffers,ni=s->image_count*sizeof *descriptor_workspace.images,nt=s->texel_count*sizeof *descriptor_workspace.texels;
    if(!s->dmemo_infos){s->dmemo_infos=calloc(1,nb+ni+nt+1);if(!s->dmemo_infos)die("Vulkan descriptor memo");}
    if(s->dmemo_set&&s->dmemo_batch==binding_cache_generation&&!memcmp(s->dmemo_infos,descriptor_workspace.buffers,nb)&&!memcmp(s->dmemo_infos+nb,descriptor_workspace.images,ni)&&!memcmp(s->dmemo_infos+nb+ni,descriptor_workspace.texels,nt)){
        descriptor_memo_hits++;dm_cpu_phase_end(DM_CPU_PHASE_DESCSET,descset_phase);return s->dmemo_set;}
    VkDescriptorSet set=descriptor_cache_find(s->layout,writes,s->count);
    if(!set){set=descriptor_allocate(s->layout);for(unsigned i=0;i<s->count;i++)writes[i].dstSet=set;
        if(s->count)vkUpdateDescriptorSets(device,s->count,writes,0,NULL);descriptor_cache_store(s->layout,set);}
    memcpy(s->dmemo_infos,descriptor_workspace.buffers,nb);memcpy(s->dmemo_infos+nb,descriptor_workspace.images,ni);memcpy(s->dmemo_infos+nb+ni,descriptor_workspace.texels,nt);
    s->dmemo_set=set;s->dmemo_batch=binding_cache_generation;
    dm_cpu_phase_end(DM_CPU_PHASE_DESCSET,descset_phase);
    return set;
}
#include "nvn_vulkan_dependencies.inc"
static int shader_host_writes(const shader_entry *s) {
    for(unsigned i=0;i<s->count;i++){const shader_binding *b=&s->bindings[i];if((b->kind==2&&(b->flags&1))||(b->kind==4&&(b->type&255)==2&&(b->flags&4)))return 1;}return 0;
}
static void dispatch_compute(const p_dispatch *d) {
    snapshot_prepare();graphics_pass_end();
    if(!state.program||!state.program->nstage)die("Vulkan dispatch without program");shader_entry *s=program_shader(state.program,0);if(s->stage!=5)die("Vulkan dispatch with graphics shader");
    synccheck_writer=synccheck_reader=s->key;dependencies_prepare_shader(s);
    // 与固定着色器同寿命复用计算管线；批次资源只包含本次绑定，不销毁共享管线。
    // Keep compute pipelines for the shader lifetime; batch resources hold bindings, not shared pipeline ownership.
    if(!s->compute_pipeline){resource_create_start creation=resource_create_begin();VkDescriptorSetLayout sets[2]={bl_layout,s->layout};VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=bl_set?2:1,.pSetLayouts=bl_set?sets:&s->layout};VK_CHECK(vkCreatePipelineLayout(device,&li,NULL,&s->compute_layout));
        VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=s->module,.pName="main"},.layout=s->compute_layout};
#ifdef __ANDROID__
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required;
        if(!dm_vk_driver_prepare_shader_stage(&vk_api,&ci.stage,&required))die("Vulkan compute subgroup policy: %s",dm_vk_driver_error(&vk_api));
#endif
        VK_CHECK(pipeline_create_compute(&ci,&s->compute_pipeline));resource_create_end(2,creation,s->key,NULL);pso_record_compute(s);}
    post_op *const *output_scope=post_copy_tail;forward_scope_marker=output_scope;
    uint32_t dynamic_offsets[256];unsigned dynamic_count=0;
    VkDescriptorSet set=bind_shader(s,NULL,dynamic_offsets,&dynamic_count);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,s->compute_pipeline);if(bl_set)vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,s->compute_layout,0,1,&bl_set,0,NULL);   /* internal compute passes bind set 0 too */
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,s->compute_layout,bl_set?1:0,1,&set,dynamic_count,dynamic_offsets);graphics_probe_dispatch(d);
    if(passtime_state>0){passtime_fs=s->key;passtime_pass_begin((unsigned)(async_current-async_batches),command,(passtime_key){0,0,0,0,0});}   /* timed as a "dispatch" kind */
    vkCmdDispatch(command,d->x,d->y,d->z);
    if(live_exp_has("csbarrier")){VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,NULL,0,NULL);}   /* live word csbarrier (experiment): a full barrier after every dispatch */
    if(passtime_state>0)passtime_pass_end((unsigned)(async_current-async_batches),command,1);PASSLOG("dispatch %s %ux%ux%u",s->key,d->x,d->y,d->z);forward_record_outputs(output_scope,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if(inplace_on()){VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,NULL,0,NULL);}   /* in place: the dispatch's writes are read in place by what follows */
    graphics_probe_cs_outputs(s);
    // 图像消费者由 image_access 精确同步；仅真实缓冲写回需要主机可见性屏障。
    // image_access synchronizes image consumers precisely; only actual buffer writebacks need host visibility.
    if(shader_host_writes(s)){VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);}
}

#include "nvn_vulkan_draw.inc"

// 驱动专用着色器变体：离线 build_shaders.py 为特定驱动产出语义等价的替代模块，运行期只在该驱动上换用。
// Driver-specific shader variants: build_shaders.py emits semantics-preserving replacement modules offline for a
// specific driver, and the runtime swaps them in on that driver only.
//
// 着色器缓存目录里的 driver-variants.tsv（格式严格，任何一行不合规都直接终止，不论它属于哪个驱动，在任何驱动上（包括
// Turnip）都一样：清单损坏就是着色器缓存损坏）：
// driver-variants.tsv in the shader cache directory (strict format; any malformed line aborts, whichever driver it is for,
// and on every driver, Turnip included: a damaged manifest means a damaged shader cache):
//   # DMVKVARIANT2
//   V\t<id>\t<VkDriverId>\t<描述 | description>
//   K\t<16 位十六进制键 | 16-hex key>\t<变体 SHA-256>\t<基础 .spv SHA-256>\t<基础 .meta SHA-256>
//                                     | <variant SHA-256>\t<base .spv SHA-256>\t<base .meta SHA-256>
// K 行归属于它前面最近的 V 行。只有实际选中驱动的 driverID 等于 V 行声明值时该变体才启用；启用后清单里的键一律
// 读 <key>.<id>.spv，并且核对三份摘要：变体本身、它所派生自的基础模块、共用的反射 .meta。数据包导入不清理旧文件，
// 所以旧包留下的清单与变体可能配上新包的基础模块（不同翻译版本或子组档位）；基础摘要不符即终止，不会把旧变体
// 配到新反射上执行。缺文件或摘要不符一律终止，不退回基础模块（基础模块正是在这个驱动上出问题的那份）；
// 预热路径也一样（它对普通模块是非致命读取）。
// K lines belong to the closest preceding V line. A variant is enabled only when the selected driver's driverID equals
// the V line's value; then every listed key loads <key>.<id>.spv and three digests are checked: the variant itself, the
// base module it was derived from, and the shared .meta reflection. Package import does not remove old files, so a
// manifest and variants left by an earlier package can meet the base modules of a newer one (another translation or
// subgroup profile); a base digest mismatch aborts instead of running an old variant against new reflection. A missing
// file or any digest mismatch aborts rather than falling back to the base module (the base module is exactly what fails
// on this driver), on the warm path too (which reads ordinary modules non-fatally).
// 变体 ID 与清单摘要进入管线缓存身份，换变体就换驱动缓存命名空间。
// The variant ID and the manifest digest enter the pipeline-cache identity, so a different variant gets a different
// driver-cache namespace.
// 构建期变体生成失败时留下 driver-variants.failed（打包拒收）；运行期见到它也终止：那份缓存不完整。
// A failed variant build leaves driver-variants.failed (packaging refuses it); the runtime aborts on it as well, since
// that cache is incomplete.
#define SHADER_VARIANT_MANIFEST "driver-variants.tsv"
#define SHADER_VARIANT_FAILED "driver-variants.failed"
#define SHADER_VARIANT_HEADER "# DMVKVARIANT2"
#define SHADER_VARIANT_MAX_KEYS 8192
typedef struct { char key[17]; uint8_t sha256[32],base_sha256[32],meta_sha256[32]; } shader_variant_key;
typedef struct { unsigned section; shader_variant_key k; } shader_variant_row;
static struct {
    int initialized,active;uint32_t driver_id;char id[32];unsigned count;
    shader_variant_key *keys;uint8_t manifest_sha256[32];
} shader_variant;
static int shader_variant_hex(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;}
static int shader_variant_key_compare(const void *a,const void *b){return strcmp(((const shader_variant_key*)a)->key,((const shader_variant_key*)b)->key);}
static int shader_variant_row_compare(const void *a,const void *b){
    const shader_variant_row *x=a,*y=b;if(x->section!=y->section)return x->section<y->section?-1:1;return strcmp(x->k.key,y->k.key);
}
static int shader_variant_id_valid(const char *id){
    size_t n=strlen(id);if(!n||n>=sizeof shader_variant.id)return 0;
    for(size_t i=0;i<n;i++)if(!((id[i]>='a'&&id[i]<='z')||(id[i]>='0'&&id[i]<='9')))return 0;
    return 1;
}
// 64 个小写十六进制字符 -> 32 字节；不合规返回 0。| 64 lowercase hex characters -> 32 bytes; 0 when malformed.
static int shader_variant_digest(const char *text,uint8_t out[32]){
    for(unsigned i=0;i<32;i++){int high=shader_variant_hex(text[2*i]),low=shader_variant_hex(text[2*i+1]);if(high<0||low<0)return 0;out[i]=(uint8_t)(high<<4|low);}
    return 1;
}
#ifdef __ANDROID__
// 在 Vulkan 设备建好、驱动不会再回退之后，管线缓存初始化之前调用一次。
// Call once after the Vulkan device exists and the driver can no longer fall back, before the pipeline cache is initialized.
static void shader_variant_init(uint32_t driver_id) {
    if(shader_variant.initialized)die("Vulkan shader variants initialized twice");
    shader_variant.initialized=1;shader_variant.driver_id=driver_id;
    const char *dir=getenv("DM_VK_SHADERS");char path[1024];
    if(dir&&snprintf(path,sizeof path,"%s/%s",dir,SHADER_VARIANT_FAILED)>=(int)sizeof path)die("Vulkan shader directory path too long for driver variants: %s",dir);
    if(!dir){
        LOG("[vulkan-shader-variant] manifest=none driver_id=%u reason=no_shader_directory",driver_id);
        if(driver_id==VK_DRIVER_ID_QUALCOMM_PROPRIETARY)LOG("[vulkan-shader-variant] 【红】Qualcomm proprietary driver selected without DM_VK_SHADERS: no driver variants can be used");
        return;
    }
    FILE *marker=fopen(path,"rb");
    if(marker||errno!=ENOENT){if(marker)fclose(marker);die("Vulkan shader cache %s: driver variant generation failed or is unreadable (%s); rebuild the shader cache",dir,SHADER_VARIANT_FAILED);}
    snprintf(path,sizeof path,"%s/%s",dir,SHADER_VARIANT_MANIFEST);
    FILE *file=fopen(path,"rb");
    if(!file){
        if(errno!=ENOENT)die("Vulkan shader variant manifest open: %s",strerror(errno));
        LOG("[vulkan-shader-variant] manifest=none driver_id=%u (cache built without driver variants; base modules on every driver)",driver_id);
        if(driver_id==VK_DRIVER_ID_QUALCOMM_PROPRIETARY)LOG("[vulkan-shader-variant] 【红】Qualcomm proprietary driver selected but the shader cache has no driver variants; rebuild the cache with build_shaders.py (driver variant qcom1) or select the bundled Turnip");
        return;
    }
    if(fseek(file,0,SEEK_END))die("Vulkan shader variant manifest seek");
    long size=ftell(file);
    if(size<=0||size>(4l<<20))die("Vulkan shader variant manifest size %ld",size);
    rewind(file);
    char *text=malloc((size_t)size+1);if(!text)die("Vulkan shader variant manifest allocation");
    if(fread(text,1,(size_t)size,file)!=(size_t)size)die("Vulkan shader variant manifest read");
    fclose(file);text[size]=0;
    dm_android_sha256_context digest;dm_android_sha256_init(&digest);dm_android_sha256_update(&digest,text,(uint32_t)size);dm_android_sha256_final(shader_variant.manifest_sha256,&digest);
    if(strlen(text)!=(size_t)size||text[size-1]!='\n')die("Vulkan shader variant manifest is not newline-terminated text");
    // 整份清单先完整校验（所有段的键行、摘要与段内重复），再按 driverID 挑段。
    // Validate the whole manifest first (key lines, digests and in-section duplicates of every section), then pick by driverID.
    shader_variant_row *rows=calloc(SHADER_VARIANT_MAX_KEYS,sizeof *rows);if(!rows)die("Vulkan shader variant key allocation");
    unsigned nrows=0,variants=0,line_number=0,selected_section=0;int selected=0;char selected_id[32]={0};uint32_t declared_ids[64];
    for(char *line=text,*next;*line;line=next){
        char *end=strchr(line,'\n');*end=0;next=end+1;line_number++;
        if(line_number==1){
            if(!strcmp(line,"# DMVKVARIANT1"))die("Vulkan shader variant manifest is format 1 (no base binding); rebuild the shader cache");
            if(strcmp(line,SHADER_VARIANT_HEADER))die("Vulkan shader variant manifest header");
            continue;
        }
        if(line[0]=='V'&&line[1]=='\t'){
            char *id=line+2,*tab=strchr(id,'\t');if(!tab)die("Vulkan shader variant manifest line %u",line_number);*tab=0;
            char *number=tab+1,*after;errno=0;unsigned long declared=strtoul(number,&after,10);
            if(!shader_variant_id_valid(id)||errno||after==number||*number<'0'||*number>'9'||*after!='\t'||declared>0xffffffffu)die("Vulkan shader variant manifest line %u",line_number);
            if(variants==sizeof declared_ids/sizeof *declared_ids)die("Vulkan shader variant manifest has too many variants");
            for(unsigned i=0;i<variants;i++)if(declared_ids[i]==declared)die("Vulkan shader variant manifest declares two variants for driver %lu",declared);
            declared_ids[variants++]=(uint32_t)declared;
            if(declared==driver_id){selected=1;selected_section=variants;snprintf(selected_id,sizeof selected_id,"%s",id);}
            continue;
        }
        if(line[0]=='K'&&line[1]=='\t'){
            if(!variants)die("Vulkan shader variant manifest key before variant line %u",line_number);
            const char *key=line+2;
            if(strlen(key)!=16+3*65||key[16]!='\t'||key[16+65]!='\t'||key[16+130]!='\t')die("Vulkan shader variant manifest line %u",line_number);
            if(nrows==SHADER_VARIANT_MAX_KEYS)die("Vulkan shader variant manifest has too many keys");
            shader_variant_row *row=&rows[nrows++];row->section=variants;
            for(unsigned i=0;i<16;i++)if(shader_variant_hex(key[i])<0)die("Vulkan shader variant manifest key line %u",line_number);
            memcpy(row->k.key,key,16);row->k.key[16]=0;
            if(!shader_variant_digest(key+17,row->k.sha256)||!shader_variant_digest(key+17+65,row->k.base_sha256)||!shader_variant_digest(key+17+130,row->k.meta_sha256))
                die("Vulkan shader variant manifest digest line %u",line_number);
            continue;
        }
        die("Vulkan shader variant manifest line %u",line_number);
    }
    free(text);
    if(nrows)qsort(rows,nrows,sizeof *rows,shader_variant_row_compare);
    for(unsigned i=1;i<nrows;i++)if(rows[i-1].section==rows[i].section&&!strcmp(rows[i-1].k.key,rows[i].k.key))die("Vulkan shader variant manifest lists %s twice in variant %u",rows[i].k.key,rows[i].section);
    if(!selected){free(rows);LOG("[vulkan-shader-variant] manifest=present variants=%u driver_id=%u active=none",variants,driver_id);return;}
    unsigned count=0;for(unsigned i=0;i<nrows;i++)count+=rows[i].section==selected_section;
    if(!count)die("Vulkan shader variant %s lists no modules",selected_id);
    shader_variant_key *keys=calloc(count,sizeof *keys);if(!keys)die("Vulkan shader variant key allocation");
    for(unsigned i=0,j=0;i<nrows;i++)if(rows[i].section==selected_section)keys[j++]=rows[i].k;   // 已按键排序 | already sorted by key
    free(rows);
    shader_variant.active=1;shader_variant.keys=keys;shader_variant.count=count;snprintf(shader_variant.id,sizeof shader_variant.id,"%s",selected_id);
    char hex[65];for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",shader_variant.manifest_sha256[i]);
    LOG("[vulkan-shader-variant] manifest=present variants=%u driver_id=%u active=%s modules=%u manifest_sha256=%s",variants,driver_id,shader_variant.id,count,hex);
}
#endif
static const shader_variant_key *shader_variant_lookup(const char *key) {
    if(!shader_variant.active)return NULL;
    shader_variant_key probe;snprintf(probe.key,sizeof probe.key,"%s",key);
    return bsearch(&probe,shader_variant.keys,shader_variant.count,sizeof *shader_variant.keys,shader_variant_key_compare);
}
// 核对变体与它派生自的那一代基础模块与反射；全部相符返回 NULL，否则返回不符的那一项。
// Check a variant against the base module and reflection generation it was derived from; NULL when all match,
// otherwise the item that differs.
static const char *shader_variant_verify(const shader_variant_key *v,const void *variant,size_t variant_bytes,const void *base,size_t base_bytes,const void *meta,size_t meta_bytes) {
    const struct { const void *data; size_t bytes; const uint8_t *want; const char *what; } items[3]={
        {variant,variant_bytes,v->sha256,"variant module"},{base,base_bytes,v->base_sha256,"base module"},{meta,meta_bytes,v->meta_sha256,"base reflection (.meta)"}};
    for(unsigned i=0;i<3;i++){
        if(items[i].bytes>0xffffffffu)return items[i].what;
        uint8_t digest[32];dm_android_sha256_context context;dm_android_sha256_init(&context);dm_android_sha256_update(&context,items[i].data,(uint32_t)items[i].bytes);dm_android_sha256_final(digest,&context);
        if(memcmp(digest,items[i].want,32))return items[i].what;
    }
    return NULL;
}
// 读一个清单列出的键的变体：变体与基础模块一律致命读取（read 是致命读取函数），三份摘要不符即终止，绝不退回基础模块。
// 返回变体字节（调用方 free）。抽成函数是为了宿主测试能用桩读取函数覆盖这条分支。
// Read the variant of a manifest-listed key: variant and base module are read fatally (read is the fatal reader), any
// digest mismatch aborts, and it never falls back to the base module. Returns the variant bytes (the caller frees them).
// It is a function so the host test can cover this branch with a stub reader.
static void *shader_variant_load(const shader_variant_key *v,const char *key,const void *meta,size_t meta_bytes,size_t *bytes,void *(*read)(const char*,const char*,size_t*)) {
    char variant_suffix[48];snprintf(variant_suffix,sizeof variant_suffix,"%s.spv",shader_variant.id);
    size_t base_bytes;void *base=read(key,"spv",&base_bytes);void *code=read(key,variant_suffix,bytes);
    const char *mismatch=shader_variant_verify(v,code,*bytes,base,base_bytes,meta,meta_bytes);free(base);
    if(mismatch)die("Vulkan shader variant %s.%s: %s does not match driver-variants.tsv (variant files from another shader cache: rebuild the shader cache, or delete driver-variants.tsv and the *.%s.spv files from the shader directory before importing a package again)",key,variant_suffix,mismatch,shader_variant.id);
    return code;
}
// 管线缓存身份：每个变体各自一个命名空间。| Pipeline-cache identity: every variant gets its own namespace.
// 未启用时不改摘要：其它驱动（Turnip）的命名空间与旧版逐位相同，已有驱动缓存继续命中。
// Leave the digest untouched when inactive: other drivers (Turnip) keep exactly the old namespace and existing caches still hit.
static void shader_variant_identity(dm_android_sha256_context *digest) {
    static const char tag[]="shader-variant=";
    if(!shader_variant.active)return;
    dm_android_sha256_update(digest,tag,sizeof tag);dm_android_sha256_update(digest,shader_variant.id,(uint32_t)strlen(shader_variant.id));
    dm_android_sha256_update(digest,shader_variant.manifest_sha256,32);
}

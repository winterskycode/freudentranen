// 真实设备时间戳报告；samples 的计数仍由 CPU 按序发布，不允许提前 GPU 转发。
// Real device timestamp reports; samples counts still publish on the CPU in order and cannot be forwarded early to the GPU.
#include "counter_timestamp_spv.h"
static VkDescriptorSetLayout counter_gpu_set_layout;
static VkPipelineLayout counter_gpu_pipeline_layout;
static VkPipeline counter_gpu_pipeline;
static uint32_t counter_gpu_parameters[4];
static int counter_gpu_initialized,counter_gpu_supported;
static uint64_t counter_gpu_reports,counter_gpu_fallbacks,counter_gpu_sample_reports;
static void counter_gpu_initialize(void) {
    if(counter_gpu_initialized)return;counter_gpu_initialized=1;
    float period=(float)gpu_timestamp_period;uint32_t bits;memcpy(&bits,&period,sizeof bits);
    unsigned exponent=(bits>>23)&255,shift=exponent<=148?148-exponent:96;
    // 精确 float32 周期转整数有理数；拒绝无法表示的范围，不能近似或截断高位。
    // Decode the exact float32 period into an integer ratio; reject unsupported ranges rather than approximate or truncate high bits.
    counter_gpu_supported=!(bits>>31)&&exponent&&exponent<=148&&shift<=95&&gpu_timestamp_bits&&gpu_timestamp_bits<=64&&gpu_timestamp_bits+26<=64+shift;
    LOG("[vulkan-counter-gpu] supported=%d timestamp_bits=%u period_bits=%#x period_ns=%.9g CPU_fallback_keeps_wait=1",counter_gpu_supported,gpu_timestamp_bits,bits,(double)period);
    if(!counter_gpu_supported)return;
    counter_gpu_parameters[0]=((bits&0x7fffff)|0x800000)*3;
    counter_gpu_parameters[1]=shift;counter_gpu_parameters[2]=gpu_timestamp_bits;counter_gpu_parameters[3]=0;
    VkDescriptorSetLayoutBinding bindings[2]={
        {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo set_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    VK_CHECK(vkCreateDescriptorSetLayout(device,&set_info,NULL,&counter_gpu_set_layout));
    VkPushConstantRange push={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=sizeof counter_gpu_parameters};
    VkPipelineLayoutCreateInfo layout_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&counter_gpu_set_layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    VK_CHECK(vkCreatePipelineLayout(device,&layout_info,NULL,&counter_gpu_pipeline_layout));
    VkShaderModule module;VkShaderModuleCreateInfo module_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=DM_VK_COUNTER_TIMESTAMP_SPV_BYTES,.pCode=dm_vk_counter_timestamp_spv};
    VK_CHECK(vkCreateShaderModule(device,&module_info,NULL,&module));
    // 普通单线程助手不使用游戏 Maxwell warp 的 64 线程适配约束。
    // This ordinary single-invocation helper does not require the game's Maxwell warp adaptation.
    VkComputePipelineCreateInfo pipeline_info={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=counter_gpu_pipeline_layout};
    VK_CHECK(pipeline_create_compute(&pipeline_info,&counter_gpu_pipeline));vkDestroyShaderModule(device,module,NULL);
}
/* Reports inside a render pass only write the timestamp; the query copy and the conversion dispatch (both outside a
   pass) are deferred to the next pass end or commit (counter_gpu_flush). Until then the post is not "encoded", so GPU
   consumers take the safe path; CPU publication happens at completion, after the flush. The counter pool of every batch is
   reset at its start (mtl_sub_begin, the queries used since the last reset), so no per-report reset is needed. */
static struct { post_op *post, *raw; uint32_t query; } counter_deferred[ASYNC_QUERY_CAPACITY];
static unsigned counter_deferred_n;
static uint64_t counter_gpu_deferred;
static void counter_gpu_batch_begin(async_batch *batch) {
    counter_gpu_initialize(); counter_deferred_n = 0;
    if (!counter_gpu_supported || !batch) return;
    /* a new pool is reset whole; after that only the queries the slot's previous batch wrote (a handful, not 1024) */
    uint32_t reset = batch->counter_written;
    if (!batch->counter_queries) { VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = ASYNC_QUERY_CAPACITY};
        VK_CHECK(vkCreateQueryPool(device, &info, NULL, &batch->counter_queries)); reset = ASYNC_QUERY_CAPACITY; }
    batch->counter_written = 0;
    if (reset) vkCmdResetQueryPool(command, batch->counter_queries, 0, reset);
}
static void counter_gpu_convert(post_op *post, post_op *raw, uint32_t query) {
    async_batch *batch = async_current;
    vkCmdCopyQueryPoolResults(command,batch->counter_queries,query,1,raw->source.buffer,0,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT);
    VkBufferMemoryBarrier raw_barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=raw->source.buffer,.size=8};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,1,&raw_barrier,0,NULL);
    VkDescriptorSet set=descriptor_allocate(counter_gpu_set_layout);
    VkDescriptorBufferInfo buffers[2]={{raw->source.buffer,0,8},{post->output.buffer,0,16}};
    VkWriteDescriptorSet writes[2]={
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffers[0]},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffers[1]}};
    vkUpdateDescriptorSets(device,2,writes,0,NULL);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,counter_gpu_pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,counter_gpu_pipeline_layout,0,1,&set,0,NULL);
    vkCmdPushConstants(command,counter_gpu_pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof counter_gpu_parameters,counter_gpu_parameters);
    vkCmdDispatch(command,1,1,1);
    /* TOTK: NVNcounterData = {counter, timestamp}; timestamp reports write counter 0 (the helper writes the timestamp
       to both words). The game's clear-once gating (SetRenderEnableConditional) depends on that 0. */
    if(post->value!=NVN_COUNTER_TYPE_SAMPLES_PASSED){
        VkBufferMemoryBarrier order={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=post->output.buffer,.size=16};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&order,0,NULL);
        vkCmdFillBuffer(command,post->output.buffer,0,8,0);
    }
    // GPU 完成之外还需明确主机读可见性；后续 GPU 消费由 forward 屏障排序。
    // Completion also needs explicit host-read visibility; forward barriers order later GPU consumers.
    VkBufferMemoryBarrier report_barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=post->output.buffer,.size=16};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&report_barrier,0,NULL);
    post->counter_timestamp_encoded=1;
    if(post->value!=NVN_COUNTER_TYPE_SAMPLES_PASSED){post->gpu_stage=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;post->gpu_access=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;post->counter_gpu_encoded=1;}
}
/* outside a render pass: record the deferred conversions */
static void counter_gpu_flush(void) {
    for (unsigned i = 0; i < counter_deferred_n; i++) counter_gpu_convert(counter_deferred[i].post, counter_deferred[i].raw, counter_deferred[i].query);
    counter_deferred_n = 0;
}
static int counter_gpu_record(post_op *post) {
    if(post->value!=NVN_COUNTER_TYPE_TIMESTAMP&&post->value!=NVN_COUNTER_TYPE_TIMESTAMP_TOP&&post->value!=NVN_COUNTER_TYPE_SAMPLES_PASSED){counter_gpu_fallbacks++;return 0;}
    counter_gpu_initialize();async_batch *batch=async_current;
    static int cpu=-1;if(cpu<0)cpu=exp_env("cpucounter");   /* TOTK_VK_EXP=cpucounter: every report takes the CPU path (A/B: what the GPU timestamps, waiting query copies and conversion dispatches cost) */
    if(cpu||!counter_gpu_supported||!batch||!batch->counter_queries||batch->counter_used==ASYNC_QUERY_CAPACITY||((uintptr_t)post->destination&7)||!mtl_addr((uintptr_t)post->destination,16,NULL,NULL)){
        counter_gpu_fallbacks++;return 0;
    }
    uint32_t query=batch->counter_used++;
    /* TOTK_VK_COUNTER_TOP=1: the game's end-of-work timestamps written at the top of the pipe too (Turnip's bottom-of-pipe
       timestamp is a wait-for-idle of the whole GPU before the counter read; the top one is a bare counter read) */
    static int top=-1;if(top<0){const char *e=getenv("TOTK_VK_COUNTER_TOP");top=e?atoi(e):0;}
    vkCmdWriteTimestamp(command,top||post->value==NVN_COUNTER_TYPE_TIMESTAMP_TOP?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,batch->counter_queries,query);
    post_op *raw=post_new(3);raw->source=buffer_create(8);post->output=buffer_create(16);
    if(post->value==NVN_COUNTER_TYPE_SAMPLES_PASSED)counter_gpu_sample_reports++;else counter_gpu_reports++;
    if(graphics_active_frame){counter_deferred[counter_deferred_n++]=(typeof(counter_deferred[0])){post,raw,query};counter_gpu_deferred++;return 1;}
    counter_gpu_convert(post,raw,query);
    return 1;
}
static void counter_gpu_report(void) {
    static uint64_t frame,reports,fallbacks,samples;
    if(!presents||presents%60||presents==frame)return;uint64_t span=presents-frame;
    LOG("[vulkan-counter-gpu] frame=%llu span=%llu reports=%.3f/frame CPU_fallback=%.3f/frame type1_CPU_samples=%.3f/frame composed=%.3f/frame composed_bytes=%.1f/frame overlays=%.3f/frame GPU_report_publication=same_16_bytes fallback_scope=legacy_timestamp type1_timestamp=device",
        (unsigned long long)presents,(unsigned long long)span,(counter_gpu_reports-reports)/(double)span,(counter_gpu_fallbacks-fallbacks)/(double)span,
        (counter_gpu_sample_reports-samples)/(double)span,
        (forward_compose_count-forward_compose_report_count)/(double)span,(forward_compose_bytes-forward_compose_report_bytes)/(double)span,(forward_compose_overlays-forward_compose_report_overlays)/(double)span);
    frame=presents;reports=counter_gpu_reports;fallbacks=counter_gpu_fallbacks;samples=counter_gpu_sample_reports;
    forward_compose_report_count=forward_compose_count;forward_compose_report_bytes=forward_compose_bytes;forward_compose_report_overlays=forward_compose_overlays;
}

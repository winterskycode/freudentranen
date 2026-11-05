// GPU consumers reuse prior outputs; indices, uniforms and texture deswizzling that require guest CPU data still wait for real completion.
static uint64_t forward_read_bytes,forward_seed_bytes;
/* zero-copy reads in place / reads of zero-copy pools that still snapshot */
static uint64_t zc_bound_calls,zc_bound_bytes,zc_unbound_calls;
#include "nvn_vulkan_forward_compose.inc"
/* the last forward_readonly_buffer answered with the pool itself (memoable) */
static int zc_last_in_place;
static uint64_t forward_memo_hits;
static VkDescriptorBufferInfo forward_readonly_buffer(uintptr_t address,uint64_t bytes,uint64_t alignment,VkPipelineStageFlags stage,VkAccessFlags access) {
    zc_last_in_place=0;forward_slice slice;forward_result result=forward_find(address,bytes,alignment,forward_scope_marker,&slice);
    if(fwd_watch(address,bytes))LOG("[fwdwatch] frame %llu read %#llx+%llu stage %#x: forward %d%s",(unsigned long long)presents,(unsigned long long)address,(unsigned long long)bytes,stage,result,
        result==FORWARD_FULL?(slice.host_written?" (host-written copy)":" (GPU output)"):"");
    if(snapshot_diag_source_current){snapshot_diag_forward=result;snapshot_diag_current=snapshot_diag_source(snapshot_diag_source_current,address,bytes);if(snapshot_diag_current)snapshot_diag_current->forward[result]++;}
    if(result==FORWARD_FULL){
        forward_read_barrier(&slice,stage,access);forward_hold(&slice);forward_read_bytes+=bytes;return (VkDescriptorBufferInfo){slice.buffer,slice.offset,bytes};
    }
    if(result!=FORWARD_NONE&&forward_compose_possible(address,bytes,forward_scope_marker))return forward_compose_buffer(address,bytes,stage,access,forward_scope_marker);
    /* TOTK_VK_ZEROCOPY_BIND=1: a range in a zero-copy pool with no pending copy / GPU output / host effect is read in place */
    static int zc_bind=-1;if(zc_bind<0){const char *e=getenv("TOTK_VK_ZEROCOPY_BIND");zc_bind=e&&atoi(e);}
    if(zc_bind&&result==FORWARD_NONE){uint64_t offset;VkBuffer pool=zc_buffer_at(address,bytes,&offset);
        if(pool&&!(alignment&&offset%alignment)&&!async_copy_pending(address,bytes)&&!async_unknown(address,bytes)){zc_bound_calls++;zc_bound_bytes+=bytes;zc_last_in_place=1;return (VkDescriptorBufferInfo){pool,offset,bytes};}
        if(pool)zc_unbound_calls++;}
    vk_buffer buffer=snapshot_buffer(address,bytes);return (VkDescriptorBufferInfo){buffer.buffer,0,bytes};
}
/* The same binding read in place last time, with no pending write registered or published since (pp_all_gen,
   pp_unknown_gen) and the same pools: the same answer, without the forward and pending-page lookups. */
static VkDescriptorBufferInfo forward_readonly_buffer_m(fwd_memo *m,uintptr_t address,uint64_t bytes,uint64_t alignment,VkPipelineStageFlags stage,VkAccessFlags access) {
    if(inplace_on()){uint64_t off;VkBuffer pool=zc_buffer_at(address,bytes,&off);if(pool&&!(alignment&&off%alignment)){inplace_ssbo++;return (VkDescriptorBufferInfo){pool,off,bytes};}inplace_fallback++;inplace_why[pool?IF_SSBO_ALIGN:IF_SSBO_POOL]++;if(!pool)inplace_classify(address,bytes);}
    uint64_t pools=atomic_load(&pool_adds),sa=pp_range_sig(&pp_all,address,bytes),su=pp_range_sig(&pp_unknown,address,bytes);
    if(m&&m->valid&&m->address==address&&m->bytes==bytes&&m->gen_all==sa&&m->gen_unknown==su&&m->pools==pools){forward_memo_hits++;zc_bound_calls++;zc_bound_bytes+=bytes;return m->info;}
    VkDescriptorBufferInfo r=forward_readonly_buffer(address,bytes,alignment,stage,access);
    if(m){if(zc_last_in_place){*m=(fwd_memo){address,bytes,sa,su,pools,r,1};}else m->valid=0;}
    return r;
}
// Tag only the confirmed read-only shader paths; original forward lookup and consumer synchronization still execute once each.
static VkDescriptorBufferInfo forward_readonly_shader_buffer(fwd_memo *m,const shader_entry *shader,const shader_binding *binding,unsigned element,unsigned slot,unsigned handle,unsigned role,uint64_t bound_bytes,uintptr_t address,uint64_t bytes,uint64_t alignment,VkPipelineStageFlags stage,VkAccessFlags access) {
    if(!snapshot_diag_frame||bytes<=65536)return forward_readonly_buffer_m(m,address,bytes,alignment,stage,access);
    snapshot_source source={shader->key,shader->stage,shader->set,binding->binding,element,binding->kind,binding->flags,binding->cb_slot,binding->cb_offset,slot,handle,role,bound_bytes};
    const snapshot_source *previous_source=snapshot_diag_source_current;snapshot_diag_row *previous_row=snapshot_diag_current;forward_result previous_forward=snapshot_diag_forward;
    snapshot_diag_source_current=&source;snapshot_diag_current=NULL;
    VkDescriptorBufferInfo result=forward_readonly_buffer(address,bytes,alignment,stage,access);
    snapshot_diag_source_current=previous_source;snapshot_diag_current=previous_row;snapshot_diag_forward=previous_forward;return result;
}
/* Returns 1 when the seed was read on the CPU (output->mapped holds it now), 0 when the GPU fills it. *inherit: for a
   GPU-filled seed from one earlier write that kept its CPU baseline, that baseline over this range (the original bytes
   before this chain of GPU writes, so a changed-blocks publish covers every writer of the chain). */
static int forward_seed(vk_buffer *output,uintptr_t address,uint64_t bytes,VkPipelineStageFlags stage,const uint8_t **inherit) {
    forward_slice slice;forward_result result=(bytes&3)?FORWARD_NONE:forward_find(address,bytes,4,forward_scope_marker,&slice);
    if(inherit)*inherit=NULL;
    if(fwd_watch(address,bytes))LOG("[fwdwatch] frame %llu seed %#llx+%llu stage %#x: forward %d%s",(unsigned long long)presents,(unsigned long long)address,(unsigned long long)bytes,stage,result,
        result==FORWARD_FULL?(slice.host_written?" (host-written copy)":" (GPU output)"):"");
    if(result==FORWARD_FULL){
        if(inherit&&slice.producer&&slice.producer->seed&&address>=(uintptr_t)slice.producer->destination&&address-(uintptr_t)slice.producer->destination+bytes<=slice.producer->size)
            *inherit=slice.producer->seed+(address-(uintptr_t)slice.producer->destination);
        graphics_pass_end();forward_read_barrier(&slice,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);forward_hold(&slice);
        VkBufferCopy copy={.srcOffset=slice.offset,.size=bytes};vkCmdCopyBuffer(command,slice.buffer,output->buffer,1,&copy);forward_seed_bytes+=bytes;
        VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=output->buffer,.size=bytes};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,stage,0,0,NULL,1,&barrier,0,NULL);return 0;
    }
    if(result!=FORWARD_NONE&&forward_compose_possible(address,bytes,forward_scope_marker)){
        forward_compose_into(*output,address,bytes,stage,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,forward_scope_marker);return 0;
    }
    if(snapshot_read(address,output->mapped,bytes))die("Vulkan storage initial contents unreadable");
    return 1;
}
static void forward_record_outputs(post_op *const *scope,VkPipelineStageFlags stage) {
    for(post_op *post=*scope;post;post=post->copy_next)if(!post->source.buffer&&post->destination&&post->size)
        forward_mark_gpu_recorded(post,stage,VK_ACCESS_SHADER_WRITE_BIT,1);
}

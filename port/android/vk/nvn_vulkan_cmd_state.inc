// Clear Vulkan command state at each command-buffer begin; identical state needs no repeated driver commands.
typedef struct graphics_command_state {
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSet sets[2];
    uint32_t dynamic_offsets[256];unsigned dynamic_count;
    VkViewport viewport;
    VkRect2D scissor;
    float blend[4],bias_units,bias_factor,bias_clamp;
    uint32_t stencil_compare[2],stencil_write[2],stencil_reference[2];
    VkBuffer vertices[16],index;
    VkDeviceSize offsets[16],index_offset;
    VkIndexType index_type;
    /* extended dynamic state, valid bit 1024 */
    ds_st eds_depth; poly_st eds_polygon;
    uint32_t valid;
} graphics_command_state;
static graphics_command_state graphics_commands;
static uint64_t graphics_command_emitted,graphics_command_reused;
static void graphics_command_reset(void) {memset(&graphics_commands,0,sizeof graphics_commands);chunk_epoch++;}
static void graphics_command_apply(VkPipeline pipeline,VkPipelineLayout layout,const VkDescriptorSet sets[2],
    const uint32_t *dynamic_offsets,unsigned dynamic_count,
    const VkViewport *viewport,const VkRect2D *scissor,const float blend[4],float units,float factor,const VkStencilOpState faces[2]) {
    graphics_command_state *last=&graphics_commands;
    if(last->pipeline!=pipeline){vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);last->pipeline=pipeline;graphics_command_emitted++;}else graphics_command_reused++;
    // Dynamic offsets are part of the binding state: comparing sets alone would reuse the previous positions.
    /* the global arrays, set 0 of every layout: once per command buffer */
    if(last->layout!=layout||memcmp(last->sets,sets,sizeof last->sets)||last->dynamic_count!=dynamic_count||memcmp(last->dynamic_offsets,dynamic_offsets,dynamic_count*sizeof *dynamic_offsets)){if(bl_set&&!(last->valid&2048)){vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&bl_set,0,NULL);last->valid|=2048;}
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,bl_set?1:0,2,sets,dynamic_count,dynamic_offsets);last->layout=layout;memcpy(last->sets,sets,sizeof last->sets);last->dynamic_count=dynamic_count;memcpy(last->dynamic_offsets,dynamic_offsets,dynamic_count*sizeof *dynamic_offsets);graphics_command_emitted++;}else graphics_command_reused++;
    if(!(last->valid&1)||memcmp(&last->viewport,viewport,sizeof *viewport)){vkCmdSetViewport(command,0,1,viewport);last->viewport=*viewport;graphics_command_emitted++;}else graphics_command_reused++;
    if(!(last->valid&2)||memcmp(&last->scissor,scissor,sizeof *scissor)){vkCmdSetScissor(command,0,1,scissor);last->scissor=*scissor;graphics_command_emitted++;}else graphics_command_reused++;
    if(!(last->valid&4)||memcmp(last->blend,blend,sizeof last->blend)){vkCmdSetBlendConstants(command,blend);memcpy(last->blend,blend,sizeof last->blend);graphics_command_emitted++;}else graphics_command_reused++;
    /* NVN's polygon offset clamp (SetPolygonOffsetClamp), where the device has depthBiasClamp (enabled with it) */
    float clamp=gpu_features.depthBiasClamp?state.polygon_offset.clamp:0;
    if(!(last->valid&8)||last->bias_units!=units||last->bias_factor!=factor||last->bias_clamp!=clamp){vkCmdSetDepthBias(command,units,clamp,factor);last->bias_units=units;last->bias_factor=factor;last->bias_clamp=clamp;graphics_command_emitted++;}else graphics_command_reused++;
    for(unsigned face=0;face<2;face++){
        VkStencilFaceFlags mask=face?VK_STENCIL_FACE_BACK_BIT:VK_STENCIL_FACE_FRONT_BIT;
        uint32_t compare_bit=16u<<face,write_bit=64u<<face,reference_bit=256u<<face;
        if(!(last->valid&compare_bit)||last->stencil_compare[face]!=faces[face].compareMask){vkCmdSetStencilCompareMask(command,mask,faces[face].compareMask);last->stencil_compare[face]=faces[face].compareMask;graphics_command_emitted++;}else graphics_command_reused++;
        if(!(last->valid&write_bit)||last->stencil_write[face]!=faces[face].writeMask){vkCmdSetStencilWriteMask(command,mask,faces[face].writeMask);last->stencil_write[face]=faces[face].writeMask;graphics_command_emitted++;}else graphics_command_reused++;
        if(!(last->valid&reference_bit)||last->stencil_reference[face]!=faces[face].reference){vkCmdSetStencilReference(command,mask,faces[face].reference);last->stencil_reference[face]=faces[face].reference;graphics_command_emitted++;}else graphics_command_reused++;
    }
    last->valid|=1023;
}
static void graphics_command_vertices(unsigned slot,VkBuffer buffer,VkDeviceSize offset) {
    if(slot>=16)die("Vulkan command vertex slot");graphics_command_state *last=&graphics_commands;
    if(last->vertices[slot]!=buffer||last->offsets[slot]!=offset){vkCmdBindVertexBuffers(command,slot,1,&buffer,&offset);last->vertices[slot]=buffer;last->offsets[slot]=offset;graphics_command_emitted++;}else graphics_command_reused++;
}
static void graphics_command_index(VkBuffer buffer,VkDeviceSize offset,VkIndexType type) {
    graphics_command_state *last=&graphics_commands;
    if(last->index!=buffer||last->index_offset!=offset||last->index_type!=type){vkCmdBindIndexBuffer(command,buffer,offset,type);last->index=buffer;last->index_offset=offset;last->index_type=type;graphics_command_emitted++;}else graphics_command_reused++;
}
/* Extended dynamic state (nvn_vulkan.c eds_on): depth / stencil, cull mode, front face, depth-bias enable, as the
   pipeline would have baked them in (graphics_pipeline_create); sent only when they change. */
static void graphics_command_eds(ds_st d,poly_st p) {
    graphics_command_state *last=&graphics_commands;
    if((last->valid&1024)&&!memcmp(&last->eds_depth,&d,sizeof d)&&!memcmp(&last->eds_polygon,&p,sizeof p)){graphics_command_reused++;return;}
    dv_eds_depth_test(command,d.dt);dv_eds_depth_write(command,d.dw);dv_eds_depth_compare(command,d.df?d.df-1:VK_COMPARE_OP_ALWAYS);dv_eds_stencil_test(command,d.st);
    dv_eds_stencil_op(command,VK_STENCIL_FACE_FRONT_BIT,d.fail_f?d.fail_f-1:VK_STENCIL_OP_KEEP,d.zpass_f?d.zpass_f-1:VK_STENCIL_OP_KEEP,d.zfail_f?d.zfail_f-1:VK_STENCIL_OP_KEEP,d.sf_f?d.sf_f-1:VK_COMPARE_OP_ALWAYS);
    dv_eds_stencil_op(command,VK_STENCIL_FACE_BACK_BIT,d.fail_b?d.fail_b-1:VK_STENCIL_OP_KEEP,d.zpass_b?d.zpass_b-1:VK_STENCIL_OP_KEEP,d.zfail_b?d.zfail_b-1:VK_STENCIL_OP_KEEP,d.sf_b?d.sf_b-1:VK_COMPARE_OP_ALWAYS);
    dv_eds_cull(command,p.cull);dv_eds_front(command,p.front?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE);
    if(eds2_on)dv_eds_bias_enable(command,!!p.offset_en);
    last->eds_depth=d;last->eds_polygon=p;last->valid|=1024;graphics_command_emitted++;
}

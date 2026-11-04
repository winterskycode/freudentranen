#!/bin/bash
# Compiles the Arm ASR compute passes (nvn_vulkan_asr.inc) into ../asr_*_spv.inc: the vendored headers and lock / luminance
# pyramid passes (third_party/asr), our compute entries for its fragment passes (asr_*.comp). ASR's Quality path, our
# inputs' permutation: HDR colour (1.4.3's upscale source keeps values above 1), render-size unjittered motion, depth not inverted, no sharpening. FFXM_HALF=0:
# the device is created without shaderFloat16.
set -e
cd "$(dirname "$0")"
A=../third_party/asr
F="-V --target-env vulkan1.1 -Os -I$A/gpu -I$A/gpu/fsr2 -DFFXM_GPU=1 -DFFXM_GLSL=1 -DFFXM_HALF=0
   -DFFXM_FSR2_OPTION_HDR_COLOR_INPUT=1 -DFFXM_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1
   -DFFXM_FSR2_OPTION_JITTERED_MOTION_VECTORS=0 -DFFXM_FSR2_OPTION_INVERTED_DEPTH=0 -DFFXM_FSR2_OPTION_APPLY_SHARPENING=0
   -DFFXM_FSR2_OPTION_SHADER_OPT_BALANCED=0 -DFFXM_FSR2_OPTION_SHADER_OPT_PERFORMANCE=0
   -DFFXM_FSR2_OPTION_SHADER_OPT_ULTRA_PERFORMANCE=0"
# the luminance pyramid without subgroup quad operations (SPD's shared-memory path): a small pass, and one Vulkan feature fewer
c() { glslangValidator $F -S comp -x -o "../asr_$1_spv.inc" "$2" >/dev/null || { glslangValidator $F -S comp -o /dev/null "$2"; exit 1; }; }
F="$F -DFFXM_SPD_NO_WAVE_OPERATIONS=1" c luminance_pyramid $A/glsl/ffxm_fsr2_compute_luminance_pyramid_pass.glsl
c reconstruct asr_reconstruct.comp
c depth_clip asr_depth_clip.comp
c lock $A/glsl/ffxm_fsr2_lock_pass.glsl
c accumulate asr_accumulate.comp
glslangValidator -V --target-env vulkan1.1 -x -o ../asr_motion_spv.inc asr_motion.comp >/dev/null
glslangValidator -V --target-env vulkan1.1 -x -o ../asr_debug_spv.inc asr_debug.comp >/dev/null
echo built

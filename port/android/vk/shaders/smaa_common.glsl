// SMAA 1x as compute passes over the game's canvas (nvn_vulkan_smaa.inc): the reference shader (third_party/smaa,
// MIT) through SMAA_CUSTOM_SL. Compute shaders have no implicit derivatives, so every read is an explicit level 0, and
// the edge detection's discard (no edge in this pixel) becomes "no edges".
// Include after the pass's own bindings; a pass may define SMAASamplePoint first (the edge pass gamma-encodes with it).
layout(local_size_x = 8, local_size_y = 8) in;
layout(push_constant) uniform Push { vec4 metrics; vec4 sub; } pc;   // 1/width, 1/height, width, height; the subsample indices (T2x)
#define SMAA_RT_METRICS pc.metrics
#ifndef SMAA_PRESET_LOW
#ifndef SMAA_PRESET_MEDIUM
#ifndef SMAA_PRESET_ULTRA
#define SMAA_PRESET_HIGH
#endif
#endif
#endif
#define SMAA_CUSTOM_SL 1
#define SMAATexture2D(tex) sampler2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) textureLod(tex, coord, 0.0)
#define SMAASampleLevelZeroPoint(tex, coord) textureLod(tex, coord, 0.0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) textureLodOffset(tex, coord, 0.0, offset)
#define SMAASample(tex, coord) textureLod(tex, coord, 0.0)
#ifndef SMAASamplePoint
#define SMAASamplePoint(tex, coord) textureLod(tex, coord, 0.0)
#endif
#define SMAASampleOffset(tex, coord, offset) textureLodOffset(tex, coord, 0.0, offset)
#define SMAA_FLATTEN
#define SMAA_BRANCH
#define lerp(a, b, t) mix(a, b, t)
#define saturate(a) clamp(a, 0.0, 1.0)
#define mad(a, b, c) fma(a, b, c)
#define SMAAGather(tex, coord) textureGather(tex, coord)
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4
#define discard return float2(0.0, 0.0)
#include "SMAA.hlsl"
#undef discard

// this pass's pixel, and its centre in texture coordinates; false outside the picture
bool smaa_pixel(out ivec2 p, out vec2 texcoord) {
    p = ivec2(gl_GlobalInvocationID.xy);
    texcoord = (vec2(p) + 0.5) * pc.metrics.xy;
    return p.x < int(pc.metrics.z) && p.y < int(pc.metrics.w);
}

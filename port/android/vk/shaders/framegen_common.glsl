// framegen_interp.comp and framegen_splat.comp: the push constants, the bindings and the motion of one depth texel.
layout(push_constant) uniform P {
    // previous view-projection * inverse(current view-projection)
    mat4 reproj;
    // output w, h; z: the scale of object motion (1; 2 when the pair is two frames apart, TOTK_FG_EVAL); w: framegen_splat
    // the pass (0 nearest depth, 1 its source texel), framegen_interp above 0 when the splatted motion field is used: the
    // score of a hole's motion candidate (a share of the match threshold)
    vec4 size;
    // x: clip-space y direction (+1), y: match threshold (mean abs difference per channel), z: 1 for the debug tint, plus
    // 2 for the motion's outcome in alpha, plus 4 for the current picture alone where the previous one shows something
    // nearer, plus 8 for no no-motion candidate on what has its own motion (all framegen_interp.comp), plus 16 times the
    // depth dilation radius (in depth texels), w: 1 when the pictures are scenes (linear canvas values, sRGB-encoded
    // here)
    vec4 opt;
    // xy: the picture's share of the picture textures (picture size / texture size), z: 1 when obj_motion holds this
    // frame's object motion, w: the picture's time t
    vec4 scale;
    // x: how sharply the weights favour the better-matching candidate (the share of the threshold one weight unit
    // spans), y: scale of the current picture's own weight (the fallback where neither candidate matches), z: the scene
    // motion's head start (a share of the threshold taken off its mismatch), w: the previous picture's share scale where
    // the match is unsure (1: blended by nearness in time there, 0: the current picture alone, warped)
    vec4 tune;
} p;
layout(set = 0, binding = 0) uniform sampler2D prev_pic;
layout(set = 0, binding = 1) uniform sampler2D cur_pic;
// object motion (RG32F): last frame's normalised position, x below -9 where nothing moved itself
layout(set = 0, binding = 2) uniform sampler2D obj_motion;
layout(set = 0, binding = 3) uniform sampler2D depth;
// The motion field at the picture's time, in depth texels (framegen_splat.comp): the nearest depth that lands on each
// texel (float bits; all ones where nothing lands: disocclusion), and the depth texel it came from (x | y << 16).
layout(set = 0, binding = 5, r32ui) uniform uimage2D splat_depth;
layout(set = 0, binding = 6, r32ui) uniform uimage2D splat_src;
// The same at the previous frame's time (t = 0): the nearest depth of this frame's surfaces that were there then.
layout(set = 0, binding = 7, r32ui) uniform uimage2D splat_prev;

const uint SPLAT_NONE = 0xffffffffu;

// Whether what is at uv pu in this frame moved itself and has its own motion (object motion drawn there).
bool own_motion(vec2 pu) {
    ivec2 os = textureSize(obj_motion, 0);
    return texelFetch(obj_motion, clamp(ivec2(pu * vec2(os)), ivec2(0), os - 1), 0).x > -9.0;
}
// Where the surface at uv pu (depth d there) was in the previous frame, minus pu (normalized picture units). The sky (and
// cleared depth) is at the far plane: reprojected there it follows the camera's turn, and the camera's own movement is
// negligible against that distance.
vec2 texel_motion(vec2 pu, float d) {
    if (p.scale.z != 0.0) {
        ivec2 os = textureSize(obj_motion, 0);
        vec4 o = texelFetch(obj_motion, clamp(ivec2(pu * vec2(os)), ivec2(0), os - 1), 0);
        if (o.x > -9.0) return (vec2(o.x * 0.5 + 0.5, 0.5 - 0.5 * p.opt.x * o.y) - pu) * p.size.z;
    }
    d = min(d, 1.0);
    vec4 c = p.reproj * vec4(pu.x * 2.0 - 1.0, p.opt.x * (1.0 - pu.y * 2.0), d * 2.0 - 1.0, 1.0);
    if (c.w <= 1e-7) return vec2(0.0);
    vec2 n = c.xy / c.w;
    return vec2(n.x * 0.5 + 0.5, 0.5 - 0.5 * p.opt.x * n.y) - pu;
}

#version 450
// Object motion (nvn_vulkan_motion.inc): the motion vertex variant writes last frame's clip position to location 15
// (translator patch 0009). Out: last frame's position divided by w, and this fragment's window position; the consumer
// turns the two into motion in its own convention.
layout(location = 15) in vec4 prev_clip;
layout(location = 0) out vec4 motion;
void main() {
    motion = vec4(prev_clip.xy / prev_clip.w, gl_FragCoord.xy);
}

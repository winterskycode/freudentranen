#!/bin/sh
# Compile the backend's own compute shaders (port/android/vk/shaders/*.comp) to SPIR-V word arrays included by the C code
# (<name>_spv.inc next to them in port/android/vk). Run after editing a shader; the .inc files are committed.
set -e
VK=$(cd "$(dirname "$0")/../vk" && pwd)
GLSLC=${GLSLC:-$(command -v glslc || echo /opt/android-sdk/ndk/29.0.14206865/shader-tools/linux-x86_64/glslc)}
for src in "$VK"/shaders/*.comp; do
    name=$(basename "$src" .comp)
    # Arm ASR's passes need its FFXM_* defines and the third_party/asr/gpu include tree: their .inc files are built
    # separately with glslangValidator
    case $name in asr_*) continue ;; esac
    "$GLSLC" -O --target-env=vulkan1.1 -I "$VK/third_party/fsr1" -I "$VK/third_party/smaa" -mfmt=num -o "$VK/${name}_spv.inc" "$src"
    echo "built ${name}_spv.inc"
done
python3 "$(dirname "$0")/gen_fsrfg.py" > /dev/null && echo "built fsrfg_passes_spv.inc"

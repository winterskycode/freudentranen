AMD FidelityFX SDK v1.1.4 (tag `v1.1.4`, commit c6efa6b, MIT, see LICENSE.txt): the GLSL of FSR 3.1 frame interpolation and
optical flow, unchanged, without the debug view. `port/android/tools/gen_fsrfg.py` compiles it into
`port/android/vk/fsrfg_passes_spv.inc`. Do not update this from the SDK's 2.x tree: its licence is not plain MIT.

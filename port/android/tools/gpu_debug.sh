#!/bin/sh
# Turn the Vulkan validation layer on (on) or off (off) for the app; the layer must be in the APK
# (TOTK_VK_VALIDATION=1 tools/build_apk.sh --install). Messages go to logcat with the tag VALIDATION.
case "$1" in
  on)  adb shell settings put global enable_gpu_debug_layers 1; adb shell settings put global gpu_debug_app dev.tok.totk
       adb shell settings put global gpu_debug_layers VK_LAYER_KHRONOS_validation; echo "validation on" ;;
  renderdoc) adb shell settings put global enable_gpu_debug_layers 1; adb shell settings put global gpu_debug_app dev.tok.totk
       adb shell settings put global gpu_debug_layers VK_LAYER_RENDERDOC_Capture; echo "renderdoc on" ;;
  off) adb shell settings delete global enable_gpu_debug_layers; adb shell settings delete global gpu_debug_app
       adb shell settings delete global gpu_debug_layers; echo "validation off" ;;
  *) echo "usage: gpu_debug.sh on|renderdoc|off"; exit 2 ;;
esac

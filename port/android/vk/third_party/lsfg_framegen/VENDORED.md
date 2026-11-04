lsfg-vk framegen library, vendored from https://github.com/PancakeTAS/lsfg-vk at tag v1.0.0 (commit 7113d7d, MIT, see
LICENSE.md). It holds no shaders: Lossless Scaling's shaders are the user's licensed data, made on the device from their
own Lossless.dll (port/android/host/lsfg_translate.cpp) and loaded at runtime from the device's files/lsfg directory.
Local changes are marked "TOTK:":
- volk initialised with the app's Vulkan loader instead of the system libvulkan (instance.cpp);
- same-device mode: initializeShared / adoptImage (lsfg.cpp, instance.cpp, device.cpp, image.cpp) run LSFG on the app's
  instance, device and queue with the app's images; ordering by submission order (presentContext with inSem -1);
- copy-in mode (createContext with in0 == in1, context.cpp, Utils::copyIn): the app hands in one picture and LSFG copies
  it into inputs of its own each frame;
- semaphore waits at ALL_COMMANDS instead of TOP_OF_PIPE, which waits for nothing (commandbuffer.cpp);
- images zeroed at creation (LSFG reads some before writing them) (image.cpp);
- debug_dump.cpp: every image's contents to files, to compare runs stage by stage (tools/lsfg_test);
- constant buffers need a memory type that is both host-visible and coherent (upstream accepted either flag) (buffer.cpp).
volk (../volk) is from the same tag's submodule (zeux/volk be3dbd4, MIT).

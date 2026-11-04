# TOTK native runtime for Android: MacNativeGames' runtime and NVN front end (android branch) + compat layer + Android
# host pieces. Configure with the NDK toolchain:
#   cmake -G Ninja -S port/android -B work/android/v143/build-host -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DTOTK_VERSION=v143
#   (API 33, the oldest of our devices: the AYN Thor runs Android 13; build_apk.sh sets the same minimum for the APK)
cmake_minimum_required(VERSION 3.22)
project(totk_android C CXX ASM)

set(TOTK_VERSION v143 CACHE STRING "game version: v143 (Tears of the Kingdom 1.4.3), botw160 / botw190 (Breath of the Wild)")
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(MNG_DIR ${ROOT}/MacNativeGames CACHE PATH "the MacNativeGames tree (another checkout, e.g. an upstream sync worktree)")
set(GAME ${MNG_DIR}/games/totk)
set(GEN ${ROOT}/work/android/${TOTK_VERSION}/tree/build)
set(DEPS ${CMAKE_CURRENT_SOURCE_DIR}/deps/out)
string(TOUPPER ${TOTK_VERSION} VERSION_UPPER)

# The game: a Tears of the Kingdom version (v143: TOTK_GAME_V143, game_patches_v143.c) or Breath of the Wild (botw160 /
# botw190: GAME_BOTW160 / GAME_BOTW190, which game_version.h turns into GAME_BOTW; game_patches_botw.c). Everything else
# -- the runtime, the NVN layer, the host -- is shared; game_version.h carries each game's addresses and data folder.
if(TOTK_VERSION MATCHES "^botw")
  set(GAME_PATCHES game_patches_botw.c aoc_romfs.c)   # the add-on content volumes (runtime/aoc_romfs.c) are Breath of the Wild.s; game_patches.c holds Tears of the Kingdom addresses
  set(GAME_DEFINE GAME_BOTW=1 GAME_${VERSION_UPPER}=1)   # both, as the Mac Makefile passes them: files that do not include game_version.h test GAME_BOTW
else()
  set(GAME_PATCHES game_patches_v143.c)
  set(GAME_DEFINE TOTK_GAME_${VERSION_UPPER}=1)
endif()
set(RUNTIME_C loader.c ${GAME_PATCHES} world_survey.c game_slot.c services.c romfs.c threads.c hle_stack.c gfx_bridge.c
    account.c input.c hid_feedback.c motion_fusion.c autoload.c gfx_probe.c memory_probe.c shared_font.c ipc.c audio_renderer.c
    audio_mixer.c audio_reverb.c audio_effects.c fs_host.c opus_codec.c movie_host.c time_host.c applet_host.c swkbd_host.c game_data.c
    settings_service.c)   # the "set" IPC service (nn::settings), which Breath of the Wild's SDK 7 asks the service manager for
list(TRANSFORM RUNTIME_C PREPEND ${GAME}/runtime/)
file(GLOB GFX_C ${GAME}/third_party/d2r_gfx/gfx/nvn/*.c ${GAME}/third_party/d2r_gfx/gfx/tex/*.c)
# Used only by the Metal backend and the macOS front end (CoreGraphics text, IOKit GPU load).
list(REMOVE_ITEM GFX_C ${GAME}/third_party/d2r_gfx/gfx/nvn/fps_overlay.c ${GAME}/third_party/d2r_gfx/gfx/nvn/totk_sysload.c)
set(MACHO_ASM ${GAME}/runtime/gate.S ${GAME}/runtime/hle_stack.S ${GAME}/runtime/thread_enter.S
    ${GAME}/third_party/d2r_gfx/gfx/nvn/nvn_entry.S ${GAME}/third_party/d2r_gfx/gfx/nvn/nvn_thunks.S)

# Mach-O assembly -> ELF assembly (tools/macho2elf.py), regenerated when the source changes
set(ELF_ASM)
foreach(src ${MACHO_ASM})
  get_filename_component(name ${src} NAME)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/elf-asm/${name})
  add_custom_command(OUTPUT ${out} COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/macho2elf.py ${src} ${out}
                     DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/tools/macho2elf.py VERBATIM)
  list(APPEND ELF_ASM ${out})
endforeach()

file(GLOB COMPAT_C ${CMAKE_CURRENT_SOURCE_DIR}/compat/src/*.c)
set(HOST ${CMAKE_CURRENT_SOURCE_DIR}/host)
set(HOST_COMMON ${HOST}/context_entry.S ${HOST}/vi.c)

set(SOURCES ${RUNTIME_C} ${GFX_C} ${ELF_ASM} ${COMPAT_C} ${HOST_COMMON})
set(INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/compat/include ${CMAKE_CURRENT_SOURCE_DIR}/host ${GEN}/${TOTK_VERSION} ${GEN}
    ${GAME}/runtime ${GAME}/third_party/d2r_gfx/gfx/nvn ${GAME}/third_party/d2r_gfx/gfx/nvn/include
    ${GAME}/third_party/d2r_gfx/gfx/tex ${GAME}/third_party/d2r_gfx/host ${DEPS}/include ${DEPS}/include/opus)
set(DEFINES ${GAME_DEFINE})
# -O3, no stack protector, no fortified string functions, no PLT: measured 2026-10-31 in the still scene against -O2 with the
# NDK's defaults (-fstack-protector-strong -D_FORTIFY_SOURCE=2): translation thread 48.8 -> 45.9 M instructions a frame,
# vkrec 21.4 -> 18.7. The fortify wrappers alone were ~7% of the replay loop's own time (line profile).
set(CFLAGS -std=gnu11 -O3 -g -Wall -Wno-unused-function -Wno-unused-parameter -Wno-deprecated-declarations
    -fvisibility=hidden -march=armv8.2-a+crypto+sha2 -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-plt
    -include ${CMAKE_CURRENT_SOURCE_DIR}/compat/include/apple_compat.h)
# Extra C flags for experiments (cmake -DTOTK_CFLAGS_EXTRA="-O3 -fno-stack-protector"): appended, so they override the above
# and the NDK toolchain's defaults (-fstack-protector-strong -D_FORTIFY_SOURCE=2, -O3 which our -O2 overrides).
set(TOTK_CFLAGS_EXTRA "" CACHE STRING "extra C flags appended for the game code and the Vulkan backend")
if(TOTK_CFLAGS_EXTRA)
    separate_arguments(TOTK_CFLAGS_EXTRA_LIST UNIX_COMMAND "${TOTK_CFLAGS_EXTRA}")
    list(APPEND CFLAGS ${TOTK_CFLAGS_EXTRA_LIST})
endif()

# The game code and host.
add_library(totk_core OBJECT ${SOURCES})
target_include_directories(totk_core PRIVATE ${INCLUDES})
target_compile_definitions(totk_core PRIVATE ${DEFINES})
target_compile_options(totk_core PRIVATE $<$<COMPILE_LANGUAGE:C>:${CFLAGS}>)
set_target_properties(totk_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
set(LIBS ${DEPS}/lib/libopus.a ${DEPS}/lib/libvpx.a log android aaudio m dl)

# The Vulkan backend (port/android/vk, adapted from the D2R Android port).
file(GLOB VK_C ${CMAKE_CURRENT_SOURCE_DIR}/vk/*.c)
add_library(totk_vk OBJECT ${VK_C})
target_include_directories(totk_vk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vk ${CMAKE_CURRENT_SOURCE_DIR}/vk/third_party ${INCLUDES})
target_compile_definitions(totk_vk PRIVATE ${DEFINES})
target_compile_options(totk_vk PRIVATE $<$<COMPILE_LANGUAGE:C>:${CFLAGS} -Wunused-function>)   # our code: unused functions warn

# The in-game overlay (vk/overlay.cpp): Dear ImGui (vk/third_party/imgui, MIT) and its Vulkan renderer, functions loaded at run time.
set(IMGUI ${CMAKE_CURRENT_SOURCE_DIR}/vk/third_party/imgui)
add_library(totk_overlay OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/vk/overlay.cpp ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp ${IMGUI}/imgui_impl_vulkan.cpp)
target_include_directories(totk_overlay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vk ${IMGUI} ${GAME}/runtime)
target_compile_definitions(totk_overlay PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_compile_options(totk_overlay PRIVATE -O2 -fvisibility=hidden -fno-exceptions -fno-rtti)

# The app: GameActivity (NativeActivity) loads libtotk.so.
# LSFG frame generation (vk/lsfg_bridge.cpp): the lsfg-vk v1.0.0 framegen library (vk/third_party/lsfg_framegen, MIT)
# and volk (MIT). It throws C++ exceptions (caught in the bridge), so it is built with them. No shaders here: the startup
# screen makes them from the player's Lossless.dll (libtotklsfg below).
set(LSFG ${CMAKE_CURRENT_SOURCE_DIR}/vk/third_party/lsfg_framegen)
file(GLOB LSFG_SOURCES ${LSFG}/src/*/*.cpp ${LSFG}/v3.1_src/*.cpp ${LSFG}/v3.1_src/*/*.cpp ${LSFG}/v3.1p_src/*.cpp ${LSFG}/v3.1p_src/*/*.cpp)
add_library(totk_lsfg OBJECT ${LSFG_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/vk/lsfg_bridge.cpp ${CMAKE_CURRENT_SOURCE_DIR}/vk/third_party/volk/volk.c)
target_include_directories(totk_lsfg PRIVATE ${LSFG}/include ${LSFG}/public ${LSFG}/v3.1_include ${LSFG}/v3.1p_include
    ${CMAKE_CURRENT_SOURCE_DIR}/vk/third_party/volk ${CMAKE_CURRENT_SOURCE_DIR}/vk)
target_compile_definitions(totk_lsfg PRIVATE VK_NO_PROTOTYPES)
set_source_files_properties(${LSFG_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/vk/lsfg_bridge.cpp PROPERTIES COMPILE_OPTIONS "-std=c++20")
target_compile_options(totk_lsfg PRIVATE -O2 -fvisibility=hidden -fexceptions -frtti -Wno-everything)

add_library(totk SHARED $<TARGET_OBJECTS:totk_core> $<TARGET_OBJECTS:totk_vk> $<TARGET_OBJECTS:totk_overlay> $<TARGET_OBJECTS:totk_lsfg> ${HOST}/app.c ${HOST}/prompts_romfs.c
    ${HOST}/recorder.c ${HOST}/report.c)
target_include_directories(totk PRIVATE ${INCLUDES} ${DEPS}/include)
target_compile_definitions(totk PRIVATE ${DEFINES})
target_compile_options(totk PRIVATE $<$<COMPILE_LANGUAGE:C>:${CFLAGS} -Wunused-function>)
target_link_libraries(totk PRIVATE ${LIBS} mediandk ${DEPS}/lib/libzstd.a)
# Playing from the player's own NSPs (setup/README.md): Tears of the Kingdom only
if(NOT TOTK_VERSION MATCHES "^botw")
    set(CAPSTONE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/deps/src/capstone CACHE PATH "the pinned Capstone source (deps/build_deps.sh)")
    add_subdirectory(setup/tables EXCLUDE_FROM_ALL)
    target_sources(totk PRIVATE ${HOST}/player_files.c)
    target_include_directories(totk PRIVATE setup/container setup/patcher setup/tables)
    target_link_libraries(totk PRIVATE toktables tokpatcher tokcontainer)
    # The startup screen's Game files page (SetupNative.java): the same reader for the launcher's process. Its own library,
    # as totklsfg is, so nothing is loaded twice: the game's process never opens this one.
    add_library(totksetup SHARED ${HOST}/setup_jni.c)
    target_compile_options(totksetup PRIVATE -std=gnu11 -O2 -Wall -fvisibility=hidden -ffunction-sections -fdata-sections)
    target_link_libraries(totksetup PRIVATE tokcontainer)
    target_link_options(totksetup PRIVATE -Wl,--gc-sections -Wl,--exclude-libs,ALL)
endif()
target_link_options(totk PRIVATE -Wl,--export-dynamic-symbol=ANativeActivity_onCreate)

# The startup screen's mod installer (Mods.java): zstd through JNI, loaded in the app's main process (not the game's).
add_library(totkmods SHARED ${HOST}/mods_jni.c)
target_include_directories(totkmods PRIVATE ${DEPS}/include)
target_compile_options(totkmods PRIVATE -std=gnu11 -O2 -Wall -fvisibility=default)
target_link_libraries(totkmods PRIVATE ${DEPS}/lib/libzstd.a)
# The RomFS and mod-base.bin from the player's own files, for the same installer (setup/README.md)
if(NOT TOTK_VERSION MATCHES "^botw")
    set(TOKM_ZSTD_INCLUDE ${DEPS}/include)
    add_subdirectory(setup/modbase EXCLUDE_FROM_ALL)
    target_sources(totkmods PRIVATE ${HOST}/player_romfs_jni.c)
    target_include_directories(totkmods PRIVATE setup/container)
    target_link_libraries(totkmods PRIVATE tokmodbase tokcontainer ${DEPS}/lib/libzstd.a)
endif()

# The startup screen's Lossless Scaling setup (Lsfg.java): the shaders of the player's own Lossless.dll translated on the
# device. Its own library, loaded in the app's main process only when a file is chosen, so the game pays nothing.
# C++ exceptions are how the DXBC compiler reports a bad file; host/lsfg_translate.cpp catches them.
include(${HOST}/lsfg_translate.cmake)
add_library(totklsfg SHARED ${HOST}/lsfg_jni.cpp ${LSFG_TRANSLATE_SOURCES})
target_include_directories(totklsfg SYSTEM PRIVATE ${LSFG_TRANSLATE_INCLUDES})
target_compile_options(totklsfg PRIVATE -std=c++17 -O2 -fvisibility=hidden -fvisibility-inlines-hidden -fexceptions -frtti
    -ffunction-sections -fdata-sections -Wno-everything)
target_link_options(totklsfg PRIVATE -Wl,--gc-sections -Wl,--exclude-libs,ALL)

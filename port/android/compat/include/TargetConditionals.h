/* Android build of MacNativeGames: not an Apple target. Code paths keyed on TARGET_OS_IPHONE take the desktop branch;
   Android-specific behaviour is keyed on __ANDROID__ in the sources. */
#pragma once
#define TARGET_OS_IPHONE 0
#define TARGET_OS_IOS 0
#define TARGET_OS_OSX 0
#define TARGET_OS_MAC 0
#define TARGET_OS_SIMULATOR 0
#define TARGET_OS_TV 0
#define TARGET_OS_VISION 0
#define TARGET_OS_WATCH 0

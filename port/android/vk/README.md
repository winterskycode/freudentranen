# Vulkan backend (from the D2R Android port)

Imported on 2026-10-20 from the D2R Android port by ImL1s (`dm/android/native`, android/v2 branch at 99cac6a, published
at gitlab.com/aa22396584/d2r-mobile and mirrored at github.com/akari-sora/d2r-mobile; neither is public any more):
`nvn_vulkan.c` and its `.inc` parts, the driver loader, present rect, pending ranges and their small helpers. `d2r/`
holds the D2R host headers it was written against (`dm.h`, `nvn_dirty.h`, `nvn_pace.h`). That port is AGPL-3.0
(`LICENSE.AGPL`) and is built on MIT code by jhaizhou (jhaizhou-ops/d2r-mobile) and sydeEvans (sydeEvans/d2r-android),
whose notices stay with it (`NOTICE` at the repository root).

Modified by this project since 2026-10-20: adapted to TOTK's command records (MacNativeGames' `nvn_obj.h`) and runtime
shader translation, and changed throughout since; the git history records each change.

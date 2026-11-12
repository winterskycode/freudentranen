/* The frame rate settings, one rule for the start (host/app.c frame_settings) and the in-game menu (vk/overlay.cpp
   frame_apply). The frame rate cap counts every picture shown, generated ones included (TOTK_FPS_CAP). Frame generation
   is a method (TOTK_FG: off, homemade, fsr, lsfg) and that method's mode (TOTK_FG_MODE for Homemade: fill, 2;
   TOTK_LSFG_MODE: fill, 1.5, 2, 3, 4; FSR 3.1 always 2). A multiplier mode renders the cap divided by it, rounded down
   so the pictures shown stay within the cap (TOTK_UNCAPPED_HZ); fill renders the cap itself and adds pictures only where
   the game falls behind it. The backend takes the method as TOTK_FRAMEGEN, which its fg_parse reads back:
     0                       off
     <rate>                  homemade x2: a picture halfway between each two game frames
     fill<cap>               homemade fill: slots of the cap the game misses get a picture extrapolated from its last two
     fsr<rate>               FSR 3.1 x2
     lsfg<rate>x<mult>       LSFG x2, x3, x4
     lsfg<rate>xa<cap>       LSFG fill (rate = cap) and x1.5: as many pictures for each game frame as its time holds
                             slots of the cap (or of the display, when that is slower)
   The refresh rate is its own setting: no combination is refused or corrected. Values from older versions (builtin,
   lsfg3, ...) read as off. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void frame_rule(const char *cap_setting, const char *fg, const char *homemade_mode, const char *lsfg_mode, char uncapped_hz[16], char framegen[32]) {
    if (!fg) fg = "off";
    int cap = cap_setting && atoi(cap_setting) > 0 ? atoi(cap_setting) : 120;
    int homemade = !strcmp(fg, "homemade"), fsr = !strcmp(fg, "fsr"), lsfg = !strcmp(fg, "lsfg");
    const char *mode = homemade ? homemade_mode : lsfg ? lsfg_mode : NULL;
    int fill = mode && !strcmp(mode, "fill"), half = lsfg && mode && !strcmp(mode, "1.5");
    int mult = fill || half ? 1 : lsfg && mode && atoi(mode) >= 2 && atoi(mode) <= 4 ? atoi(mode) : 2;
    /* x1.5: two game frames for every three pictures */
    int rendered = !(homemade || fsr || lsfg) || fill ? cap : half ? cap * 2 / 3 : cap / mult;
    if (rendered < 1) rendered = 1;
    snprintf(uncapped_hz, 16, "%d", rendered);
    if (homemade && fill) snprintf(framegen, 32, "fill%d", cap);
    else if (homemade) snprintf(framegen, 32, "%d", rendered);
    else if (fsr) snprintf(framegen, 32, "fsr%d", rendered);
    else if (lsfg && (fill || half)) snprintf(framegen, 32, "lsfg%dxa%d", rendered, cap);
    else if (lsfg) snprintf(framegen, 32, "lsfg%dx%d", rendered, mult);
    else snprintf(framegen, 32, "0");
}

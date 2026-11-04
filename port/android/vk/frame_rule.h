/* The frame rate settings, one rule for the start (host/app.c frame_settings) and the in-game menu (vk/overlay.cpp
   frame_apply). The frame rate cap counts every picture shown, generated ones included (TOTK_FPS_CAP), and frame
   generation is a method with a multiplier (TOTK_FG: off, builtin (x2), fsr (x2), lsfg (x2), lsfg3, lsfg4). The game renders the
   cap divided by the multiplier, rounded down so the pictures shown stay within the cap (TOTK_UNCAPPED_HZ), and the
   backend takes the method with that rate (TOTK_FRAMEGEN: 0, <rate> for the built-in one, fsr<rate>, lsfg<rate>x<multiplier>; the
   backend's fg_parse reads it back). The refresh rate is its own setting: no combination is refused or corrected. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void frame_rule(const char *cap_setting, const char *fg, char uncapped_hz[16], char framegen[32]) {
    if (!fg) fg = "off";
    int cap = cap_setting && atoi(cap_setting) > 0 ? atoi(cap_setting) : 120, lsfg = !strncmp(fg, "lsfg", 4), adaptive = !strcmp(fg, "lsfga");
    /* adaptive (lsfga): the game renders at the cap and LSFG fills the display's refreshes with as many pictures as each frame needs */
    int fsr = !strcmp(fg, "fsr"), mult = adaptive ? 1 : !strcmp(fg, "builtin") || fsr ? 2 : lsfg ? (atoi(fg + 4) >= 2 ? atoi(fg + 4) : 2) : 1, rendered = cap / mult;
    if (rendered < 1) rendered = 1;
    snprintf(uncapped_hz, 16, "%d", rendered);
    if (adaptive) snprintf(framegen, 32, "lsfg%dxa", rendered);
    else if (mult == 1) snprintf(framegen, 32, "0");
    else if (lsfg) snprintf(framegen, 32, "lsfg%dx%d", rendered, mult);
    else if (fsr) snprintf(framegen, 32, "fsr%d", rendered);
    else snprintf(framegen, 32, "%d", rendered);
}

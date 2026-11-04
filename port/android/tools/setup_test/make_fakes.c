/* Made-up game files for SetupTest (setup/container/tests/synth.h: made-up keys, no game content): make_fakes <folder> */
#include "synth.h"
#include <stdio.h>
#include <stdlib.h>

static void put(const char *dir, const char *name, const void *data, size_t size) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, size, f) != size || fclose(f)) { perror(path); exit(1); }
}

/* The base and the update of this spec under their own names; either may be NULL. */
static void make(const char *dir, const sy_spec *s, const char *base, const char *update) {
    sy_game g;
    sy_make(&g, s);
    if (base) put(dir, base, g.base.data, g.base.size);
    if (update) put(dir, update, g.update.data, g.update.size);
    put(dir, "keys.txt", g.keys_text, g.keys_size);
    sy_free(&g);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const char *dir = argv[1];
    sy_spec s;
    sy_default(&s, 7);
    make(dir, &s, "base.nsp", "update.nsp");
    sy_default(&s, 7); s.xci = 1;
    make(dir, &s, "base.xci", NULL);
    sy_default(&s, 7); s.update_version -= 65536;
    make(dir, &s, NULL, "older-update.nsp");
    sy_default(&s, 7); s.base_version = 65536;
    make(dir, &s, "newer-base.nsp", NULL);
    sy_default(&s, 7); s.base_title = 0x01006A800016E000ull; s.update_title = 0x01006A800016E800ull;
    make(dir, &s, "other-game.nsp", "other-update.nsp");
    return 0;
}

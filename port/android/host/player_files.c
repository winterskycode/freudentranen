/* The game straight from the player's files: keys and the two packages in, the rewritten modules and the runtime tables out,
   all in memory. The same steps as tables_cli chain, which proves their result byte for byte against the PC's. */
#include "player_files.h"
#include "container.h"
#include "patcher.h"
#include "tables.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

_Noreturn void runtime_fail(const char *message);
void *dm_android_asset(const char *name, size_t *size);
/* romfs-config.txt: the size and hashes of 1.4.3's merged image and index. Constants of the game version, no content: the
   reader reproduces them from the packages (setup/tables chain --full). */
static const char romfs_config[] =
    "ROMFS_IMAGE_SIZE 17052625232\n"
    "ROMFS_IMAGE_SHA256 797e66f2efa5b99bdafe4af2d6e778a5ef2f2ca5dc3b45136dd601daea419e9b\n"
    "ROMFS_INDEX_SHA256 0531e8f1dc695e7e78745b2ee4658439ba501e056a3530f1888c91d0785e57cb\n";
static tokc_game *game;
static int base_fd = -1, update_fd = -1;
static uint8_t *tables, *image[TOKT_MODULE_COUNT];
static size_t tables_size;
static tokp_result rewrite[TOKT_MODULE_COUNT];

static double ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
static _Noreturn void fail(const char *what, const char *err) {
    static char line[400];
    snprintf(line, sizeof line, "%s: %s", what, err);
    runtime_fail(line);
}
static int open_input(const char *fd_name, const char *path_name) {
    const char *e = getenv(fd_name);
    if (e && *e) return atoi(e);
    e = getenv(path_name);
    if (!e || !*e) return -1;
    int fd = open(e, O_RDONLY | O_CLOEXEC);
    if (fd < 0) fail("game file", e);
    return fd;
}
int player_on(void) {
    const char *a = getenv("TOTK_BASE_FD"), *b = getenv("TOTK_BASE_PATH");
    return (a && *a) || (b && *b);
}
/* The index the runtime reads, written once: it serves as the cache, so every reader of it stays as it is. */
static void write_index(void) {
    const char *dir = getenv("TOTK_ROMFS");
    if (!dir || !*dir) return;
    char path[600], tmp[620], err[200];
    snprintf(path, sizeof path, "%s/runtime-index.bin", dir);
    if (!access(path, R_OK)) return;
    uint8_t *bytes; size_t size;
    if (tokc_romfs_index(game, &bytes, &size, err, sizeof err)) fail("game data index", err);
    mkdir(dir, 0700);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f || fwrite(bytes, 1, size, f) != size || fclose(f) || rename(tmp, path)) fail("game data index", "could not be written");
    free(bytes);
    fprintf(stderr, "[player] index written: %zu bytes\n", size);
}
void player_load(void) {
    if (game) return;
    char err[300];
    double t0 = ms();
    const char *keys_path = getenv("TOTK_KEYS");
    if (!keys_path || !*keys_path) runtime_fail("keys: none set");
    int keys_fd = open(keys_path, O_RDONLY | O_CLOEXEC);
    if (keys_fd < 0) runtime_fail("keys: the copy in the app's storage cannot be read");
    tokc_keys *keys;
    if (tokc_keys_parse_fd(keys_fd, &keys, err, sizeof err)) fail("keys", err);
    close(keys_fd);
    base_fd = open_input("TOTK_BASE_FD", "TOTK_BASE_PATH");
    update_fd = open_input("TOTK_UPDATE_FD", "TOTK_UPDATE_PATH");
    if (base_fd < 0 || update_fd < 0) runtime_fail("game files: the base game and the update are both needed");
    int rc = tokc_game_open(base_fd, update_fd, keys, &game, err, sizeof err);
    tokc_keys_free(keys);
    if (rc) fail("game files", err);
    double t_open = ms();
    static const tokc_module which[TOKT_MODULE_COUNT] = {TOKC_MODULE_MAIN, TOKC_MODULE_SUBSDK0, TOKC_MODULE_SDK};
    uint8_t *nso[TOKT_MODULE_COUNT]; size_t nso_size[TOKT_MODULE_COUNT];
    tokt_module m[TOKT_MODULE_COUNT];
    char hash[TOKT_MODULE_COUNT][65];
    for (int i = 0; i < TOKT_MODULE_COUNT; i++) {
        if (tokc_game_module(game, which[i], &nso[i], &nso_size[i], err, sizeof err)) fail(tokt_module_names[i], err);
        if (i == TOKT_MAIN && tokt_check_main(nso[i], nso_size[i], err, sizeof err)) fail("game version", err);
        if (tokp_rewrite_alloc(nso[i], nso_size[i], &image[i], &rewrite[i], err, sizeof err)) fail(tokt_module_names[i], err);
        tokt_sha256_hex(image[i], rewrite[i].image_size, hash[i]);
        m[i] = (tokt_module){nso[i], nso_size[i], image[i], &rewrite[i], hash[i]};
    }
    double t_code = ms();
    if (tokt_make(m, &tables, &tables_size, err, sizeof err)) fail("runtime tables", err);
    for (int i = 0; i < TOKT_MODULE_COUNT; i++) free(nso[i]);
    double t_tables = ms();
    write_index();
    fprintf(stderr, "[player] loaded from the player's files: open %.1f ms, code %.1f ms, tables %.1f ms, index %.1f ms; SHA-256 %s\n",
            t_open - t0, t_code - t_open, t_tables - t_code, ms() - t_tables, tokt_sha_impl_name());
}
const uint8_t *player_tables(uint64_t *size) { *size = tables_size; return tables; }
static int module_id(const char *name) {
    for (int i = 0; i < TOKT_MODULE_COUNT; i++) if (!strcmp(name, tokt_module_names[i])) return i;
    return -1;
}
const uint8_t *player_image(const char *name, size_t *size) {
    int i = module_id(name);
    if (i < 0 || !image[i]) return NULL;
    *size = rewrite[i].image_size;
    return image[i];
}
void player_image_free(const char *name) {
    int i = module_id(name);
    if (i < 0 || !image[i]) return;
    free(image[i]); image[i] = NULL;
    tokp_result_free(&rewrite[i]);
}
uint8_t *player_module_copy(const char *name, size_t *size) {
    static const tokc_module which[TOKT_MODULE_COUNT] = {TOKC_MODULE_MAIN, TOKC_MODULE_SUBSDK0, TOKC_MODULE_SDK};
    int i = module_id(name); char err[200]; uint8_t *nso, *copy = NULL; size_t nso_size; tokp_result r;
    if (i < 0 || !game || tokc_game_module(game, which[i], &nso, &nso_size, err, sizeof err)) return NULL;
    if (!tokp_rewrite_alloc(nso, nso_size, &copy, &r, err, sizeof err)) { *size = r.image_size; tokp_result_free(&r); }
    free(nso);
    return copy;
}
int player_image_fd(void) { return dup(update_fd); }
uint64_t player_romfs_size(void) { return tokc_romfs_size(game); }
ssize_t player_romfs_pread(void *buf, size_t n, off_t off) {
    int64_t r = tokc_romfs_pread(game, buf, n, (uint64_t)off);
    return r < 0 ? -1 : (ssize_t)r;
}
const char *player_game_file(const char *name) { return !strcmp(name, "romfs-config.txt") ? romfs_config : NULL; }
/* The firmware's six shared fonts, one font for all (tools/fetch_font.sh): the Chinese and Korean text asks for the standard one
   only, for accents, punctuation, signs and kana, and draws the rest from the game's own fonts. runtime/shared_font.c hands it
   out plain, as the console's nn::pl does: the game takes a font that starts with OTTO as it is (main+0x10e3e48). */
const uint8_t *player_shared_font(uint64_t *size) {
    size_t n = 0; uint8_t *font = dm_android_asset("fonts/SharedFont.otf", &n);
    *size = n; return font;
}

/* Playing from the player's own NSPs (setup/README.md). All of it is off unless TOTK_BASE_FD or TOTK_BASE_PATH is set, and it is
   linked for Tears of the Kingdom only: callers test the weak player_on first. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
__attribute__((weak)) int player_on(void);
__attribute__((weak)) void player_load(void);                                    /* reads, rewrites, makes the tables: once, before they are used */
__attribute__((weak)) const uint8_t *player_tables(uint64_t *size);              /* runtime-tables.bin, kept for the life of the process */
__attribute__((weak)) const uint8_t *player_image(const char *name, size_t *size);   /* <name>.patched.bin */
__attribute__((weak)) void player_image_free(const char *name);
__attribute__((weak)) uint8_t *player_module_copy(const char *name, size_t *size);   /* rewritten again, malloc'd, for the shader scan */
__attribute__((weak)) int player_image_fd(void);                                 /* a descriptor of the game's own, for romfs.c's image_fd */
__attribute__((weak)) uint64_t player_romfs_size(void);
__attribute__((weak)) ssize_t player_romfs_pread(void *buf, size_t n, off_t off);
__attribute__((weak)) const char *player_game_file(const char *name);            /* a game-data file that ships with the app, as text; NULL: the file on disk */
__attribute__((weak)) const uint8_t *player_shared_font(uint64_t *size);         /* the plain TrueType font that stands in for the firmware's six */

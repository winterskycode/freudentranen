/* Damage to a loaded set of three modules, shared by fuzz_malformed and the libFuzzer target. Each image is a copy of its exact
   size, so a read past one stops a sanitizer build. */
#ifndef TOK_TABLES_FUZZ_COMMON_H
#define TOK_TABLES_FUZZ_COMMON_H
#include "../rewritten_files.h"

/* Where damage can be aimed, found from the undamaged image. */
enum { FZ_HEAD, FZ_MOD0, FZ_DYNAMIC, FZ_SYMBOLS, FZ_NAMES, FZ_RELA, FZ_JMPREL, FZ_RELR, FZ_HASH, FZ_INIT, FZ_PLT, FZ_ALL, FZ_REGIONS };
typedef struct { uint64_t off, len; } fz_region;

typedef struct { unsigned mod; uint64_t off; uint8_t old[8]; unsigned n; } fz_undo;
#define FZ_MAX_UNDO 2048

typedef struct {
    tokt_files f[TOKT_MODULE_COUNT];
    /* f[].image, damaged and put back by fz_edit and fz_revert */
    uint8_t *image[TOKT_MODULE_COUNT];
    /* the first 0x100 bytes, exact size */
    uint8_t *nso[TOKT_MODULE_COUNT];
    size_t nso_size[TOKT_MODULE_COUNT];
    /* the working results */
    tokp_result rewrite[TOKT_MODULE_COUNT];
    tokp_gate *gates[TOKT_MODULE_COUNT];
    fz_region region[TOKT_MODULE_COUNT][FZ_REGIONS];
    char digest[TOKT_MODULE_COUNT][65];
    fz_undo undo[FZ_MAX_UNDO];
    unsigned nundo;
} fz_case;

void fz_load(fz_case *c, const char *rewritten, const char *exefs);
/* Writes width bytes of value at off in region; past the region's end it wraps to the start. */
void fz_edit(fz_case *c, unsigned mod, unsigned region, uint64_t off, unsigned width, uint64_t value);
void fz_revert(fz_case *c);
/* Puts the working results and NSO headers back. */
void fz_reset(fz_case *c);
/* tokt_make on the working state, image_bytes[i] giving the buffer size of a truncated image, or 0. Any result it accepts is
   put through the checks of runtime/loader.c load_tables; a failure there aborts. Returns its result code. */
int fz_run(fz_case *c, const uint64_t image_bytes[TOKT_MODULE_COUNT]);
#endif

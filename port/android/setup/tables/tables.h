/* The runtime tables in C: MacNativeGames' scripts/prepare_runtime.py for Tears of the Kingdom 1.4.3, the bytes of
   runtime-tables.bin that runtime/loader.c load_tables reads. Its output must stay byte-identical to the Python tool
   (regress.sh compares them).

   Buffers only, no files. The input is what setup/container and setup/patcher leave in memory: each module as stored, its
   rewritten image and the patcher's result. Nothing in them is trusted: a malformed module gives an error, never a read or
   write outside the buffers. No global state, so calls may run on separate threads. */
#ifndef TOK_TABLES_H
#define TOK_TABLES_H
#include "patcher.h"
#include <stddef.h>
#include <stdint.h>

/* The prepare_runtime.py this code mirrors. */
#define TOKT_PY_SHA256 "ed63ee8e731b2ba0a73997371b7772d428a423033617afe43ed412663a4f40f0"
#define TOKT_VERSION "v143"
/* The 1.4.3 main module as stored: the one image prepare_runtime.py accepts for this version. */
#define TOKT_MAIN_NSO_SHA256 "e84a90d551fed0824b4b74933b9c6d043c458521363a7cea58b2faca1822dc9e"
/* load_tables refuses this size and up; the real file is 16 MB */
#define TOKT_MAX_TABLES (1ull << 30)
/* game-data/rewriter.txt, whole. */
#define TOKT_REWRITER_TXT "# SHA-256 of the patch_nso.py that rewrote rewritten/" TOKT_VERSION "\nREWRITER_SHA256 " TOKP_PY_SHA256 "\n"

enum {
    TOKT_OK = 0,
    /* bad call */
    TOKT_E_ARGS = -1,
    /* a module is malformed: what is an assert or an exception in the Python tool */
    TOKT_E_FORMAT = -2,
    /* tokt_check_main: not the supported 1.4.3 */
    TOKT_E_VERSION = -3,
    /* the tables would reach TOKT_MAX_TABLES */
    TOKT_E_TOO_BIG = -4,
    TOKT_E_NOMEM = -5,
};

/* The file's order, not the one of tokc_module. */
typedef enum { TOKT_MAIN, TOKT_SUBSDK0, TOKT_SDK, TOKT_MODULE_COUNT } tokt_module_id;
extern const char *const tokt_module_names[TOKT_MODULE_COUNT];

typedef struct {
    /* the module as stored (tokc_game_module): only its header is read */
    const void *nso; size_t nso_size;
    /* the rewritten image: rewrite->image_size bytes */
    const void *image;
    const tokp_result *rewrite;
    /* the image's SHA-256 in lowercase hex if the caller knows it, or NULL to hash it here. Taken as given: it goes
       into the tables, unchecked. */
    const char *image_sha256;
} tokt_module;

/* prepare_runtime.py's version check, kept apart so that tests can feed tokt_make damaged modules: run it on the main module
   as stored before anything else. TOKT_E_VERSION when it is not the supported one. */
int tokt_check_main(const void *nso, size_t nso_size, char *err, size_t errlen);

/* runtime-tables.bin into *tables, malloc'd and the caller's to free. modules is indexed by tokt_module_id. On an error
   *tables is NULL. err, when not NULL, receives one line of text. */
int tokt_make(const tokt_module modules[TOKT_MODULE_COUNT], uint8_t **tables, size_t *size, char *err, size_t errlen);

/* SHA-256, on the arm64 instructions when the CPU has them. */
typedef struct { uint32_t h[8]; uint64_t bytes; uint8_t buf[64]; size_t have; } tokt_sha256;
void tokt_sha256_init(tokt_sha256 *s);
void tokt_sha256_update(tokt_sha256 *s, const void *data, size_t n);
void tokt_sha256_final(tokt_sha256 *s, uint8_t out[32]);
void tokt_sha256_hex(const void *data, size_t n, char hex[65]);
/* "armv8" or "portable" */
const char *tokt_sha_impl_name(void);
#endif

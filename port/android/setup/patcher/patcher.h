/* The game-code rewriter in C: port/rewriter/d2r/patch_nso.py in the shipped configuration, DM_TARGET=android DM_X18=native.
   Its output must stay byte-identical to the Python tool (regress.sh compares them). DM_X18=slot, the Apple target and the
   CUTS / OBS / PARAMS tables (empty for this game) are not ported.

   Buffers only, no files: nso is the module as stored in the ExeFS, image is where the rewritten module goes. Nothing in the
   module is trusted: a malformed one gives an error, never a read or write outside the buffers. Calls share no state, so
   the modules can be rewritten on separate threads. */
#ifndef TOK_PATCHER_H
#define TOK_PATCHER_H
#include <stddef.h>
#include <stdint.h>

/* The patch_nso.py this code mirrors. It goes into build_src.sha256 and from there into game-data/rewriter.txt. */
#define TOKP_PY_SHA256 "cd1a4bccf4842382ee0518a97eb72124f5b75bd272d1203ff36c488a4e1715d6"
#define TOKP_TARGET "android-context-v1"
#define TOKP_KEY_PAGE_SIZE 0x4000u
/* larger images are refused; the largest game module is 62 MB */
#define TOKP_MAX_IMAGE (1ull << 30)

enum {
    TOKP_OK = 0,
    /* bad call */
    TOKP_E_ARGS = -1,
    /* the module is malformed */
    TOKP_E_FORMAT = -2,
    /* well formed, but a rewrite rule refuses it (what is an assert or SystemExit in the Python tool) */
    TOKP_E_RULE = -3,
    /* image_cap is too small: the result's image_size says what it takes */
    TOKP_E_SPACE = -4,
    TOKP_E_NOMEM = -5,
    /* Capstone failed */
    TOKP_E_DECODER = -6,
};

typedef enum { TOKP_SVC, TOKP_TPIDR_R, TOKP_TPIDR_W, TOKP_TPIDRRO_R, TOKP_CNTPCT, TOKP_CNTVCT, TOKP_KIND_COUNT } tokp_kind;
/* the names in sites.tsv and gates.tsv */
extern const char *const tokp_kind_names[TOKP_KIND_COUNT];

/* A rewritten instruction. arg: the svc number, or the destination register of a timer read. */
typedef struct { uint64_t off; uint32_t kind, arg; } tokp_site;
/* A host gate: ret is the return address inside the patch area, original the instruction it replaces. Emission order. */
typedef struct { uint64_t ret, original; uint32_t kind, arg; } tokp_gate;

typedef struct {
    uint64_t text_end, ro_off, data_off;
    /* the loader fills this page */
    uint64_t key_page_off;
    /* the patch area; the image before it is the module, its key page and padding */
    uint64_t patch_off;
    /* first 16 bytes of the NSO module id, in hex */
    char build_id[33];
} tokp_layout;

typedef struct {
    tokp_layout layout;
    /* patch_off + patch_size: the size of <name>.patched.bin */
    uint64_t image_size;
    uint64_t patch_size;
    /* by address */
    tokp_site *sites; size_t nsites;
    tokp_gate *gates; size_t ngates;
    /* key page references in the patch area (the Python tool's slot_refs) */
    uint32_t slot_refs;
} tokp_result;

/* Reads the header only and checks it: the layout, so a caller can size its buffer. image_cap must hold patch_off plus the
   patch area, which is only known after the rewrite. */
int tokp_read_layout(const void *nso, size_t nso_size, tokp_layout *layout, char *err, size_t errlen);

/* Rewrites nso into image. image needs no clearing and no alignment. On TOKP_OK the first image_size bytes are the same as
   <name>.patched.bin and out must be released with tokp_result_free. On TOKP_E_SPACE out holds only the sizes, nothing to
   free. On any other error out is cleared. err, when not NULL, receives one line of text. */
int tokp_rewrite(const void *nso, size_t nso_size, void *image, size_t image_cap, tokp_result *out, char *err, size_t errlen);

/* The same into a buffer of its own: *image is malloc'd and is the caller's to free. */
int tokp_rewrite_alloc(const void *nso, size_t nso_size, uint8_t **image, tokp_result *out, char *err, size_t errlen);

void tokp_result_free(tokp_result *r);
#endif

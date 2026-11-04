/* Made-up game files for the tests: a base NSP or XCI and an update NSP with every structure the reader parses, encrypted with
   made-up keys. No game content. A test can change any plain structure before it is hashed and encrypted, so that damage
   reaches the parsers instead of stopping at a wrong hash or a failed decryption. */
#ifndef TOK_CONTAINER_SYNTH_H
#define TOK_CONTAINER_SYNTH_H
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SY_ROOT_TABLE,     /* PFS0 or HFS0 tables of the file */
    SY_TICKET,
    SY_NCA_HEADER,     /* the plain 0xC00 bytes, any NCA */
    SY_CNMT,
    SY_META_PFS,       /* the PFS0 around the content meta */
    SY_EXEFS,          /* the update's ExeFS */
    SY_HASH_TABLE,     /* its SHA-256 blocks */
    SY_BKTR_REL,
    SY_BKTR_SUB,
    SY_ROMFS_HEADER,
    SY_ROMFS_DIRS,
    SY_ROMFS_FILES,
    SY_REGION_COUNT
} sy_region;

/* Called with each plain structure just before it is hashed and encrypted; change it freely, its size stays. */
typedef void (*sy_hook)(void *user, sy_region r, uint8_t *data, size_t size);

typedef struct {
    uint64_t seed;
    uint32_t pieces;       /* ranges of the patched section; > 818 gives a second relocation bucket */
    uint32_t subsections;  /* > 1023 gives a second subsection bucket */
    int xci;               /* base as an XCI with a key area instead of a ticket */
    int base_version;      /* content meta versions, for the error tests */
    int update_version;
    uint64_t base_title, update_title;
    sy_hook hook;
    void *user;
} sy_spec;

typedef struct { uint8_t *data; size_t size; } sy_file;

typedef struct {
    sy_file base, update;
    char *keys_text; size_t keys_size;
    uint8_t *image; size_t image_size;      /* what the merged RomFS must read as */
    uint8_t *index; size_t index_size;      /* what its index must be */
    uint8_t *module[3]; size_t module_size[3];
} sy_game;

void sy_default(sy_spec *s, uint64_t seed);
void sy_make(sy_game *g, const sy_spec *s);
void sy_free(sy_game *g);

/* A descriptor holding the bytes (memfd). */
int sy_fd(const void *data, size_t size);
#endif

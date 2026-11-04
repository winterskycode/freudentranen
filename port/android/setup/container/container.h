/* The player's own game files, read in place: an NSP or XCI of the base game 1.0.0, an NSP of the 1.4.3 update and prod.keys.
   No merged image is ever written. tokc_romfs_pread gives the bytes romfs.bin would hold at the same offset, tokc_romfs_index
   the same runtime-index.bin, tokc_game_module the ExeFS modules for the rewriter (setup/patcher).

   I/O is pread on descriptors the caller opened and keeps open, so a document picker's descriptor works. Files are identified
   by what is inside them, never by name. Nothing read is trusted: a malformed file gives an error, never a read or write
   outside the buffers.

   No global state. Keys and an open game are read-only after they are made, so any number of threads may use them at once;
   only the free calls need the others to be finished. Error text names a key, never its value. Keys and title keys are
   wiped when freed. */
#ifndef TOK_CONTAINER_H
#define TOK_CONTAINER_H
#include <stddef.h>
#include <stdint.h>

#define TOKC_TITLE_BASE 0x0100F2C0115B6000ull     /* Tears of the Kingdom */
#define TOKC_TITLE_UPDATE 0x0100F2C0115B6800ull   /* and its updates */
#define TOKC_VERSION_BASE 0u                      /* 1.0.0, in the content meta */
#define TOKC_VERSION_UPDATE 720896u               /* 1.4.3 */

enum {
    TOKC_OK = 0,
    TOKC_E_ARGS = -1,          /* bad call */
    TOKC_E_IO = -2,            /* a read failed or came up short */
    TOKC_E_FORMAT = -3,        /* the file is malformed, damaged or of a kind not supported */
    TOKC_E_NOMEM = -4,
    TOKC_E_KEY_MISSING = -5,   /* prod.keys lacks a key this file needs; the text names it */
    TOKC_E_KEY_WRONG = -6,     /* the keys do not decrypt this file */
    TOKC_E_TICKET = -7,        /* no usable common ticket for a file that needs one */
    TOKC_E_WRONG_GAME = -8,
    TOKC_E_WRONG_VERSION = -9,
    TOKC_E_SWAPPED = -10,      /* the update was given as the base game, or the other way round */
};

typedef struct tokc_keys tokc_keys;
typedef struct tokc_game tokc_game;

/* prod.keys as text, from memory or a descriptor. Keys that can be made from others are made, so a file with only master
   keys and sources also works. Fails only on an unreadable file, a value of the wrong form or no header_key: the keys a
   game needs are checked when it is opened, and named if missing. */
int tokc_keys_parse(const void *text, size_t size, tokc_keys **out, char *err, size_t errlen);
int tokc_keys_parse_fd(int fd, tokc_keys **out, char *err, size_t errlen);
void tokc_keys_free(tokc_keys *keys);

typedef enum { TOKC_META_NONE = 0, TOKC_META_APPLICATION = 0x80, TOKC_META_PATCH = 0x81, TOKC_META_ADDON = 0x82 } tokc_meta_type;

typedef struct {
    uint64_t title_id;     /* from the content meta */
    uint32_t version;
    uint32_t type;         /* tokc_meta_type */
    int is_xci;
    int metas;             /* content metas found; the fields above are this game's if one is, else the first's */
} tokc_info;

/* What one file holds, for telling a player at once that they picked the wrong one. */
int tokc_inspect(int fd, const tokc_keys *keys, tokc_info *info, char *err, size_t errlen);

/* Opens the pair: base must be Tears of the Kingdom 1.0.0, update 1.4.3. Both descriptors must stay open until
   tokc_game_close. The keys may be freed after this returns. */
int tokc_game_open(int base_fd, int update_fd, const tokc_keys *keys, tokc_game **out, char *err, size_t errlen);
void tokc_game_close(tokc_game *g);

typedef enum { TOKC_MODULE_MAIN, TOKC_MODULE_SDK, TOKC_MODULE_SUBSDK0, TOKC_MODULE_COUNT } tokc_module;

/* A module of the 1.4.3 ExeFS as stored (compressed NSO): *bytes is malloc'd and the caller's to free. The SHA-256 blocks of
   the ExeFS are checked as it is read. */
int tokc_game_module(const tokc_game *g, tokc_module m, uint8_t **bytes, size_t *size, char *err, size_t errlen);

/* The size of romfs.bin. */
uint64_t tokc_romfs_size(const tokc_game *g);

/* pread on the RomFS image: up to n bytes at off into buf, any offset and size. Returns n, or less only at the end of the
   image (0 at or past it), or a negative TOKC_E_ code. */
int64_t tokc_romfs_pread(const tokc_game *g, void *buf, size_t n, uint64_t off);

/* runtime-index.bin, made from the RomFS's own tables: *bytes is malloc'd and the caller's to free. */
int tokc_romfs_index(const tokc_game *g, uint8_t **bytes, size_t *size, char *err, size_t errlen);

/* The same three for the base game's own RomFS, 1.0.0 without the update. The mod installer compares the two. */
uint64_t tokc_base_romfs_size(const tokc_game *g);
int64_t tokc_base_romfs_pread(const tokc_game *g, void *buf, size_t n, uint64_t off);
int tokc_base_romfs_index(const tokc_game *g, uint8_t **bytes, size_t *size, char *err, size_t errlen);

/* 1 when n bytes at off of the image are, by the update's table, the very bytes at base_off of the base game's RomFS: nothing
   is read, so 0 says only that the bytes must be compared. */
int tokc_romfs_same_as_base(const tokc_game *g, uint64_t off, uint64_t base_off, uint64_t n);

typedef struct {
    uint64_t relocations;     /* BKTR entries: which file an image range comes from */
    uint64_t subsections;     /* BKTR entries: which counter a range of the update uses */
    uint64_t table_bytes;     /* heap the open game keeps for them */
    uint64_t image_offset;    /* of the RomFS in the patched section */
    uint64_t base_bytes, update_bytes;   /* of the section, by where the bytes come from */
} tokc_stats;
void tokc_game_stats(const tokc_game *g, tokc_stats *out);

const char *tokc_aes_impl_name(void);   /* the AES this machine runs: "armv8", "aes-ni" or "portable" */
#endif

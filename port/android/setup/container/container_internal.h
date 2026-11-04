/* Shared by the container reader's files and its tests. */
#ifndef TOK_CONTAINER_INTERNAL_H
#define TOK_CONTAINER_INTERNAL_H
#include "container.h"
#include <stdarg.h>
#include <string.h>

/* The formats are little endian; read byte by byte so that nothing depends on alignment. */
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static inline void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static inline void wr64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }
/* a + b <= limit, without wrapping */
static inline int fits(uint64_t a, uint64_t b, uint64_t limit) { return a <= limit && b <= limit - a; }

int tokc_fail(char *err, size_t errlen, int code, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void tokc_wipe(void *p, size_t n);
/* Exactly n bytes at off; TOKC_E_IO otherwise. */
int tokc_pread_full(int fd, void *buf, size_t n, uint64_t off);

/* ---- crypto ---- */
enum { TOKC_AES_AUTO, TOKC_AES_PORTABLE };
typedef struct {
    uint8_t rk[176];   /* encryption round keys, as bytes */
    uint8_t dk[176];   /* the same for the instructions' decryption: reversed, InvMixColumns applied */
    int hw;
} tokc_aes;
void tokc_aes_init(tokc_aes *a, const uint8_t key[16], int mode);
void tokc_aes_encrypt(const tokc_aes *a, const uint8_t in[16], uint8_t out[16]);
void tokc_aes_decrypt(const tokc_aes *a, const uint8_t in[16], uint8_t out[16]);
void tokc_aes_ecb_decrypt(const uint8_t key[16], const uint8_t *in, uint8_t *out, size_t n);
/* AES-CTR in place, counter = high 8 bytes then (off / 16) big endian; any byte offset and length. */
void tokc_aes_ctr(const tokc_aes *a, const uint8_t high[8], uint64_t off, void *data, size_t n);
/* AES-XTS of one data unit from its first tweak (not yet encrypted); n is a multiple of 16. */
void tokc_xts_decrypt(const tokc_aes *data_key, const tokc_aes *tweak_key, const uint8_t tweak[16], void *data, size_t n);
void tokc_xts_encrypt(const tokc_aes *data_key, const tokc_aes *tweak_key, const uint8_t tweak[16], void *data, size_t n);
/* The sectors of an NCA header: 0x200 bytes each, big-endian sector number as the tweak. */
void tokc_nca_xts(const uint8_t key[32], void *data, size_t n, int encrypt);

/* aes_hw.c: the instructions, when compiled for them. */
int tokc_aes_hw_compiled(void);
void tokc_aes_hw_make_dk(uint8_t dk[176], const uint8_t rk[176]);
void tokc_aes_hw_encrypt(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]);
void tokc_aes_hw_decrypt(const uint8_t dk[176], const uint8_t in[16], uint8_t out[16]);
void tokc_aes_hw_ctr(const uint8_t rk[176], const uint8_t high[8], uint64_t block, uint8_t *data, size_t blocks);

typedef struct { uint32_t h[8]; uint64_t bytes; uint8_t buf[64]; size_t have; } tokc_sha256;
void tokc_sha256_init(tokc_sha256 *s);
void tokc_sha256_update(tokc_sha256 *s, const void *data, size_t n);
void tokc_sha256_final(tokc_sha256 *s, uint8_t out[32]);
void tokc_sha256_buf(const void *data, size_t n, uint8_t out[32]);

/* ---- keys ---- */
#define TOKC_GENERATIONS 32
struct tokc_keys {
    uint8_t header[32];
    uint8_t titlekek[TOKC_GENERATIONS][16], area_app[TOKC_GENERATIONS][16], master[TOKC_GENERATIONS][16];
    uint32_t have_titlekek, have_area_app, have_master;   /* a bit per generation */
    int have_header;
};

/* ---- containers ---- */
#define TOKC_NCA_HEADER 0xC00u
typedef struct { char name[64]; uint64_t off, size; } tokc_entry;   /* off: from the start of the range parsed */
/* Reads n bytes at off of some range: a file or a decrypted section. */
typedef int (*tokc_reader)(const void *ctx, void *buf, size_t n, uint64_t off);
/* PFS0 (stride 24) or HFS0 (stride 64) at off of a range of size bound. *out is malloc'd. */
int tokc_part_parse(tokc_reader read, const void *ctx, uint64_t off, uint64_t bound, const char *magic, uint32_t stride,
                    tokc_entry **out, uint32_t *count, char *err, size_t errlen);

typedef struct {
    int fd;
    uint64_t off, size;             /* in the file */
    uint8_t hdr[TOKC_NCA_HEADER];   /* decrypted */
    uint8_t content_type, area_index;
    unsigned generation;
    uint64_t title_id;
    int has_rights;
} tokc_nca;

typedef struct { uint8_t rights[16], key[16]; int personalized; } tokc_ticket;
typedef struct { uint64_t title_id; uint32_t version; uint8_t type; } tokc_meta;

typedef struct {
    int fd, is_xci;
    uint64_t size;
    tokc_nca *ncas; uint32_t nncas;
    tokc_ticket *tickets; uint32_t ntickets;
    tokc_meta *metas; uint32_t nmetas;
} tokc_package;

int tokc_package_open(tokc_package *p, int fd, const tokc_keys *keys, char *err, size_t errlen);
void tokc_package_free(tokc_package *p);

enum { FS_ROMFS = 0, FS_PFS0 = 1, HASH_SHA256 = 2, HASH_IVFC = 3, CRYPT_NONE = 1, CRYPT_XTS = 2, CRYPT_CTR = 3, CRYPT_CTR_EX = 4 };
#define NCA_FS(n, i) ((n)->hdr + 0x400 + 0x200 * (i))

/* A section of an NCA, decrypted on read. */
typedef struct {
    int fd;
    uint64_t file_off;    /* of the section in the file */
    uint64_t nca_off;     /* in the NCA: the counter counts from there */
    uint64_t size;
    uint8_t high[8];      /* the counter's fixed half */
    int encrypted;
    tokc_aes aes;
} tokc_section;

/* Whether section i is there. tokc_section_open also checks its header's hash and finds its key: the key area, or a ticket
   in one of the packages given. */
int tokc_section_present(const tokc_nca *n, int i);
int tokc_section_open(tokc_section *s, const tokc_nca *n, int i, const tokc_keys *keys, const tokc_package *const *packages,
                      int npackages, char *err, size_t errlen);
int tokc_section_read(const void *section, void *buf, size_t n, uint64_t off);   /* a tokc_reader */

/* ---- the patched RomFS ---- */
typedef struct {
    uint64_t *virt;       /* where each relocation starts in the section, ascending */
    uint64_t *phys;       /* where it starts in its source; the top bit says the update */
    uint64_t nrel, total;
    uint64_t *sub_off;    /* where each subsection starts in the update section, ascending */
    uint32_t *sub_ctr;
    uint64_t nsub, sub_total;
} tokc_bktr;
#define TOKC_BKTR_PATCH (1ull << 63)

int tokc_bktr_load(tokc_bktr *b, const tokc_section *update, const uint8_t *fs, uint64_t base_size, char *err, size_t errlen);
void tokc_bktr_free(tokc_bktr *b);

struct tokc_game {
    tokc_section base, update, exefs;
    tokc_bktr bktr;
    uint64_t image_off, image_size;   /* the RomFS in the patched section */
    uint64_t base_image_off, base_image_size;   /* and in the base game's own */
    uint64_t pfs_off, pfs_size;       /* the ExeFS in its section, and its hash table */
    uint64_t hash_off, hash_size;
    uint32_t hash_block;
    uint8_t master_hash[32];
    tokc_entry *modules;
    uint32_t nmodules;
};

/* How many source runs, each one or more preads, a range of the image takes. For measuring. */
uint64_t tokc_romfs_runs(const tokc_game *g, uint64_t off, uint64_t n);
#endif

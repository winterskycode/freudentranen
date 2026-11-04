/* A rewritten module as tools leave it on disk: <name>.patched.bin, .fixups.tsv, .gates.tsv and the stored NSO. For tests and
   for tables_cli's comparison with the Python tool. The app has all of this in memory and never needs it. */
#ifndef TOK_REWRITTEN_FILES_H
#define TOK_REWRITTEN_FILES_H
#include "tables.h"

typedef struct {
    uint8_t *nso, *image;
    size_t nso_size;
    tokp_result rewrite;
} tokt_files;

/* 0, or -1 with a line of text in err. */
int tokt_files_load(tokt_files *f, const char *rewritten, const char *exefs, const char *name, char *err, size_t errlen);
void tokt_files_free(tokt_files *f);
int tokt_read_file(const char *path, uint8_t **data, size_t *size);
#endif

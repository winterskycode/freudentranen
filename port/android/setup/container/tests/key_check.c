/* Key derivation by behaviour, on a real prod.keys: parsed as it is, and again with every titlekek, key area key and header key
   taken out so that they must be made from the master keys and sources. The two must agree. Prints only names and counts.
     key_check <prod.keys> */
#include "../container_internal.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

static int strip_line(const char *p, size_t n) {
    static const char *const names[] = {"titlekek_", "key_area_key_application_", "header_key "};
    for (int i = 0; i < 3; i++) {
        size_t l = strlen(names[i]);
        if (n > l && !strncmp(p, names[i], l) && strncmp(p, "titlekek_source", 15) && strncmp(p, "key_area_key_application_source", 31)) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    prctl(PR_SET_DUMPABLE, 0);
    if (argc != 2) return 2;
    int fd = open(argv[1], O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { perror(argv[1]); return 2; }
    size_t size = (size_t)st.st_size;
    char *text = malloc(size), *cut = malloc(size);
    if (tokc_pread_full(fd, text, size, 0)) return 2;
    size_t kept = 0;
    for (size_t at = 0; at < size;) {
        const char *nl = memchr(text + at, '\n', size - at);
        size_t n = nl ? (size_t)(nl - (text + at)) + 1 : size - at;
        if (!strip_line(text + at, n)) { memcpy(cut + kept, text + at, n); kept += n; }
        at += n;
    }
    char err[200];
    tokc_keys *a, *b;
    int rc = tokc_keys_parse(text, size, &a, err, sizeof err);
    if (rc) { printf("FAIL parse: %s\n", err); return 1; }
    rc = tokc_keys_parse(cut, kept, &b, err, sizeof err);
    if (rc) { printf("FAIL parse without the made keys: %s\n", err); return 1; }
    int bad = 0, same = 0;
    bad += memcmp(a->header, b->header, 32) != 0;
    for (int g = 0; g < TOKC_GENERATIONS; g++) {
        if (a->have_master >> g & 1) {
            if (a->have_titlekek >> g & 1) {
                if (!(b->have_titlekek >> g & 1) || memcmp(a->titlekek[g], b->titlekek[g], 16)) { printf("  titlekek_%02x differs\n", g); bad++; } else same++;
            }
            if (a->have_area_app >> g & 1) {
                if (!(b->have_area_app >> g & 1) || memcmp(a->area_app[g], b->area_app[g], 16)) { printf("  key_area_key_application_%02x differs\n", g); bad++; } else same++;
            }
        }
    }
    printf("%s keys made from master keys and sources: %d agree with the file, %d differ\n", bad ? "FAIL" : "PASS", same + !memcmp(a->header, b->header, 32), bad);
    tokc_keys_free(a); tokc_keys_free(b);
    tokc_wipe(text, size); tokc_wipe(cut, size);
    return bad != 0;
}

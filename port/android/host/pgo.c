/* Profile-guided optimisation (CMakeLists.txt TOTK_PGO=generate): the instrumented library's profile, written to
   <files>/pgo/totk.profraw. Android kills the app instead of letting it exit, so a thread writes the counters every 30 s;
   they are reset once, TOTK_PGO_RESET_S seconds in (default 90: the title and the first load are past), so the profile is
   play, not start-up. Each write holds everything counted since the reset. Other builds: nothing. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
void totk_pgo_start(void);
#ifdef TOTK_PGO_GENERATE
int __llvm_profile_write_file(void);
void __llvm_profile_set_filename(const char *name);
void __llvm_profile_reset_counters(void);
static void *pgo_thread(void *unused) {
    (void)unused;
    const char *e = getenv("TOTK_PGO_RESET_S");
    sleep(e ? (unsigned)atoi(e) : 90);
    __llvm_profile_reset_counters();
    fprintf(stderr, "[pgo] counters reset\n");
    for (unsigned n = 1;; n++) {
        sleep(30);
        int r = __llvm_profile_write_file();
        fprintf(stderr, "[pgo] profile written (%u, %s)\n", n, r ? "FAILED" : "ok");
    }
    return NULL;
}
void totk_pgo_start(void) {
    static int started;
    const char *files = getenv("TOTK_FILES_DIR");
    if (!files || started++) return;
    static char path[1024];
    snprintf(path, sizeof path, "%s/pgo", files); mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/pgo/totk.profraw", files);
    __llvm_profile_set_filename(path);
    pthread_t t; if (!pthread_create(&t, NULL, pgo_thread, NULL)) pthread_detach(t);
    fprintf(stderr, "[pgo] instrumented build: profile to %s\n", path);
}
#else
void totk_pgo_start(void) {}
#endif

/* The player's own files for the startup screen's mod installer (class dev.tok.totk.PlayerRomfs, in libtotkmods): the RomFS through
   the reader, in place of romfs.bin, and mod-base.bin made once from the two packages. Tears of the Kingdom only.
   open takes the keys' path and two descriptors that stay open; every other call needs it to have worked. Errors are
   IOExceptions with the reader's text, which names keys, never their values. */
#include "container_internal.h"
#include "modbase.h"
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_READ ((uint64_t)1 << 30)   /* a Java array holds at most 2 GiB - 1; the merge reads nothing near this */
static tokc_game *game;
static uint8_t *index_bytes;
static uint64_t file_count, strings;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void fail(JNIEnv *env, const char *what) {
    jclass c = (*env)->FindClass(env, "java/io/IOException");
    if (c) (*env)->ThrowNew(env, c, what);
}
/* Binary search, as the index is sorted by path; 1 when found */
static int find(const char *path, uint64_t *off, uint64_t *size) {
    uint64_t lo = 0, hi = file_count;
    while (lo < hi) {
        uint64_t m = lo + (hi - lo) / 2;
        const char *name = (const char *)index_bytes + strings + rd32(index_bytes + 24 + m * 24 + 16);
        int c = strcmp(name, path);
        if (!c) { *off = rd64(index_bytes + 24 + m * 24); *size = rd64(index_bytes + 24 + m * 24 + 8); return 1; }
        if (c < 0) lo = m + 1; else hi = m;
    }
    return 0;
}
JNIEXPORT void JNICALL Java_dev_tok_totk_PlayerRomfs_open(JNIEnv *env, jclass cls, jstring keys_path, jint base_fd, jint update_fd) {
    (void)cls;
    char err[300]; tokc_keys *keys = NULL; tokc_game *g = NULL; uint8_t *bytes = NULL; size_t size = 0; int ok = 0;
    const char *path = (*env)->GetStringUTFChars(env, keys_path, NULL);
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    if (path) (*env)->ReleaseStringUTFChars(env, keys_path, path);
    if (fd < 0) { fail(env, "keys: the copy in the app's storage cannot be read"); return; }
    pthread_mutex_lock(&lock);
    int rc = tokc_keys_parse_fd(fd, &keys, err, sizeof err);
    close(fd);
    if (!rc) { rc = tokc_game_open(base_fd, update_fd, keys, &g, err, sizeof err); tokc_keys_free(keys); }
    if (!rc) rc = tokc_romfs_index(g, &bytes, &size, err, sizeof err);
    if (!rc) {
        free(index_bytes); tokc_game_close(game);
        game = g; index_bytes = bytes; file_count = rd64(bytes + 8); strings = rd64(bytes + 16); ok = 1;
    } else tokc_game_close(g);
    pthread_mutex_unlock(&lock);
    if (!ok) fail(env, err);
}
JNIEXPORT jboolean JNICALL Java_dev_tok_totk_PlayerRomfs_has(JNIEnv *env, jclass cls, jstring path) {
    (void)cls;
    const char *p = (*env)->GetStringUTFChars(env, path, NULL); uint64_t off, size;
    if (!p) return JNI_FALSE;
    int found = game && find(p, &off, &size);
    (*env)->ReleaseStringUTFChars(env, path, p);
    return found ? JNI_TRUE : JNI_FALSE;
}
/* A file's bytes, or null when the RomFS lacks it */
JNIEXPORT jbyteArray JNICALL Java_dev_tok_totk_PlayerRomfs_read(JNIEnv *env, jclass cls, jstring path) {
    (void)cls;
    const char *p = (*env)->GetStringUTFChars(env, path, NULL); uint64_t off, size;
    if (!p) return NULL;
    int found = game && find(p, &off, &size);
    (*env)->ReleaseStringUTFChars(env, path, p);
    if (!found) return NULL;
    if (size > MAX_READ) { fail(env, "a game file is too big to merge"); return NULL; }
    uint8_t *b = malloc((size_t)size + 1); jbyteArray out = NULL;
    if (!b) { fail(env, "out of memory"); return NULL; }
    if (tokc_romfs_pread(game, b, (size_t)size, off) != (int64_t)size) fail(env, "a game file cannot be read");
    else if ((out = (*env)->NewByteArray(env, (jsize)size))) (*env)->SetByteArrayRegion(env, out, 0, (jsize)size, (const jbyte *)b);
    free(b);
    return out;
}
/* SHA-256 of a file, in hex; null when the RomFS lacks it. Streamed: originals can be large. */
JNIEXPORT jstring JNICALL Java_dev_tok_totk_PlayerRomfs_sha(JNIEnv *env, jclass cls, jstring path) {
    (void)cls;
    const char *p = (*env)->GetStringUTFChars(env, path, NULL); uint64_t off, size;
    if (!p) return NULL;
    int found = game && find(p, &off, &size);
    (*env)->ReleaseStringUTFChars(env, path, p);
    if (!found) return NULL;
    enum { STEP = 1 << 20 };
    uint8_t *b = malloc(STEP), digest[32]; char hex[65]; tokc_sha256 s;
    if (!b) { fail(env, "out of memory"); return NULL; }
    tokc_sha256_init(&s);
    for (uint64_t at = 0; at < size;) {
        size_t n = size - at < STEP ? (size_t)(size - at) : STEP;
        if (tokc_romfs_pread(game, b, n, off + at) != (int64_t)n) { free(b); fail(env, "a game file cannot be read"); return NULL; }
        tokc_sha256_update(&s, b, n); at += n;
    }
    free(b);
    tokc_sha256_final(&s, digest);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return (*env)->NewStringUTF(env, hex);
}
/* mod-base.bin at out: written beside it first, so that a file there is whole. On a PC 1.3 s on 16 threads, 464 MB read. */
JNIEXPORT void JNICALL Java_dev_tok_totk_PlayerRomfs_makeModBase(JNIEnv *env, jclass cls, jstring out) {
    (void)cls;
    if (!game) { fail(env, "the game files are not open"); return; }
    char err[300], path[1024], tmp[1040]; uint8_t *bytes = NULL; size_t size = 0;
    const char *p = (*env)->GetStringUTFChars(env, out, NULL);
    if (!p) return;
    snprintf(path, sizeof path, "%s", p); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    (*env)->ReleaseStringUTFChars(env, out, p);
    if (tokm_make(game, 0, NULL, NULL, &bytes, &size, err, sizeof err)) { fail(env, err); return; }
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(bytes, 1, size, f) == size;
    if (f && fclose(f)) ok = 0;
    free(bytes);
    if (!ok || rename(tmp, path)) { unlink(tmp); fail(env, "mod-base.bin could not be written"); }
}

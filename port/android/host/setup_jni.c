/* libtotksetup.so: the container reader on the startup screen's Game files page (SetupNative.java, dev.tok.totk). The launcher's
   process only, and only once that page needs it. The game's process never loads it: libtotk.so links the reader for
   host/player_files.c. Nothing here logs, and nothing it returns holds a key's value (container.h). */
#include "container.h"
#include <fcntl.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static jstring text(JNIEnv *env, char *err) {
    /* NewStringUTF wants modified UTF-8 */
    for (char *p = err; *p; p++) if ((unsigned char)*p >= 0x80) *p = '?';
    return (*env)->NewStringUTF(env, err);
}

static void wipe(void *p, size_t n) { volatile char *v = p; while (n--) *v++ = 0; }

static int open_keys(JNIEnv *env, jstring path, tokc_keys **keys, char *err, size_t errlen) {
    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    int fd = p ? open(p, O_RDONLY | O_CLOEXEC) : -1;
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
    if (fd < 0) { snprintf(err, errlen, "the app's copy of prod.keys cannot be read"); return TOKC_E_IO; }
    int rc = tokc_keys_parse_fd(fd, keys, err, errlen);
    close(fd);
    return rc;
}

JNIEXPORT jstring JNICALL Java_dev_tok_totk_SetupNative_keysError(JNIEnv *env, jclass cls, jbyteArray bytes, jint size) {
    (void)cls;
    char err[300] = "";
    char *copy = malloc(size > 0 ? (size_t)size : 1);
    if (!copy) { snprintf(err, sizeof err, "out of memory"); return text(env, err); }
    (*env)->GetByteArrayRegion(env, bytes, 0, size, (jbyte *)copy);
    tokc_keys *keys = NULL;
    int rc = (*env)->ExceptionCheck(env) ? TOKC_E_ARGS : tokc_keys_parse(copy, (size_t)size, &keys, err, sizeof err);
    tokc_keys_free(keys);
    wipe(copy, (size_t)size); free(copy);
    return rc && !(*env)->ExceptionCheck(env) ? text(env, err) : NULL;
}

JNIEXPORT jstring JNICALL Java_dev_tok_totk_SetupNative_inspect(JNIEnv *env, jclass cls, jstring keys_path, jint fd, jlongArray out) {
    (void)cls;
    char err[300] = "";
    tokc_keys *keys = NULL; tokc_info info;
    memset(&info, 0, sizeof info);
    int rc = open_keys(env, keys_path, &keys, err, sizeof err);
    if (!rc) rc = tokc_inspect(fd, keys, &info, err, sizeof err);
    tokc_keys_free(keys);
    jlong v[6] = {rc, (jlong)info.title_id, info.version, info.type, info.is_xci, info.metas};
    (*env)->SetLongArrayRegion(env, out, 0, 6, v);
    return rc ? text(env, err) : NULL;
}

JNIEXPORT jstring JNICALL Java_dev_tok_totk_SetupNative_pair(JNIEnv *env, jclass cls, jstring keys_path, jint base, jint update, jlongArray out) {
    (void)cls;
    char err[300] = "";
    tokc_keys *keys = NULL; tokc_game *game = NULL;
    int rc = open_keys(env, keys_path, &keys, err, sizeof err);
    if (!rc) rc = tokc_game_open(base, update, keys, &game, err, sizeof err);
    tokc_keys_free(keys);
    tokc_game_close(game);
    jlong v[1] = {rc};
    (*env)->SetLongArrayRegion(env, out, 0, 1, v);
    return rc ? text(env, err) : NULL;
}

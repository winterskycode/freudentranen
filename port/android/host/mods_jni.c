/* libtotkmods.so: zstd for the startup screen's mod installer (Mods.java, class dev.tok.totk.Zstd). The game's .zs files
   are zstd frames, many made with one of the dictionaries in Pack/ZsDic.pack.zs; Zstd.dictionaries registers those once
   and every call then picks the one a frame names. A merged pack is compressed again with its original's dictionary. */
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

/* MAX_OUT: no decompressed file larger than 1 GiB (a Java array holds at most 2 GiB - 1; a frame claiming more, or a
   stream growing past it, is an IOException rather than a JNI abort). LEVEL: the compression level of merged files;
   it changes only their size, every level reads the same. */
enum { MAX_DICTS = 8, LEVEL = 16 };
#define MAX_OUT ((size_t)1 << 30)
static ZSTD_DDict *ddicts[MAX_DICTS];
static ZSTD_CDict *cdicts[MAX_DICTS];
static unsigned dict_ids[MAX_DICTS], ndicts;

static jbyteArray out_array(JNIEnv *env, const void *data, size_t n) {
    jbyteArray a = (*env)->NewByteArray(env, (jsize)n);
    if (a) (*env)->SetByteArrayRegion(env, a, 0, (jsize)n, data);
    return a;
}

static void fail(JNIEnv *env, const char *what) {
    jclass c = (*env)->FindClass(env, "java/io/IOException");
    if (c) (*env)->ThrowNew(env, c, what);
}

JNIEXPORT void JNICALL Java_dev_tok_totk_Zstd_dictionaries(JNIEnv *env, jclass cls, jobjectArray dicts) {
    (void)cls;
    for (unsigned i = 0; i < ndicts; i++) { ZSTD_freeDDict(ddicts[i]); ZSTD_freeCDict(cdicts[i]); }
    ndicts = 0;
    jsize n = (*env)->GetArrayLength(env, dicts);
    for (jsize i = 0; i < n && ndicts < MAX_DICTS; i++) {
        jbyteArray d = (jbyteArray)(*env)->GetObjectArrayElement(env, dicts, i);
        jsize len = (*env)->GetArrayLength(env, d);
        jbyte *b = (*env)->GetByteArrayElements(env, d, NULL);
        ddicts[ndicts] = ZSTD_createDDict(b, (size_t)len);
        cdicts[ndicts] = ZSTD_createCDict(b, (size_t)len, LEVEL);
        dict_ids[ndicts] = ZSTD_getDictID_fromDict(b, (size_t)len);
        (*env)->ReleaseByteArrayElements(env, d, b, JNI_ABORT);
        (*env)->DeleteLocalRef(env, d);
        if (ddicts[ndicts] && cdicts[ndicts]) ndicts++;
    }
}

/* The dictionary id a frame was made with (0: none). */
JNIEXPORT jint JNICALL Java_dev_tok_totk_Zstd_dictionaryOf(JNIEnv *env, jclass cls, jbyteArray src) {
    (void)cls;
    jbyte head[18]; jsize n = (*env)->GetArrayLength(env, src); if (n > 18) n = 18;
    (*env)->GetByteArrayRegion(env, src, 0, n, head);
    return (jint)ZSTD_getDictID_fromFrame(head, (size_t)n);
}

JNIEXPORT jbyteArray JNICALL Java_dev_tok_totk_Zstd_decompress(JNIEnv *env, jclass cls, jbyteArray src) {
    (void)cls;
    jsize n = (*env)->GetArrayLength(env, src);
    jbyte *in = (*env)->GetByteArrayElements(env, src, NULL);
    unsigned id = ZSTD_getDictID_fromFrame(in, (size_t)n);
    const ZSTD_DDict *dict = NULL;
    for (unsigned i = 0; i < ndicts; i++) if (dict_ids[i] == id) dict = ddicts[i];
    jbyteArray result = NULL;
    if (id && !dict) { (*env)->ReleaseByteArrayElements(env, src, in, JNI_ABORT); fail(env, "zstd frame needs a dictionary the RomFS does not have"); return NULL; }
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (dict) ZSTD_DCtx_refDDict(dctx, dict);
    unsigned long long size = ZSTD_getFrameContentSize(in, (size_t)n);
    if (size != ZSTD_CONTENTSIZE_UNKNOWN && size != ZSTD_CONTENTSIZE_ERROR && size > MAX_OUT) {
        ZSTD_freeDCtx(dctx); (*env)->ReleaseByteArrayElements(env, src, in, JNI_ABORT); fail(env, "zstd frame is larger than 1 GiB"); return NULL;
    }
    if (size != ZSTD_CONTENTSIZE_UNKNOWN && size != ZSTD_CONTENTSIZE_ERROR) {
        void *out = malloc(size ? size : 1);
        size_t got = out ? ZSTD_decompressDCtx(dctx, out, size, in, (size_t)n) : 0;
        if (out && !ZSTD_isError(got) && got == size) result = out_array(env, out, got);
        free(out);
    } else {
        /* no size in the header: stream into a growing buffer */
        size_t cap = (size_t)n * 4 + 65536, used = 0; char *out = malloc(cap);
        ZSTD_inBuffer ib = {in, (size_t)n, 0}; size_t r = 1;
        while (out && r != 0) {
            if (used == cap) {
                if (cap >= MAX_OUT) { free(out); out = NULL; break; }
                cap = cap * 2 > MAX_OUT ? MAX_OUT : cap * 2;
                char *more = realloc(out, cap); if (!more) { free(out); out = NULL; break; } out = more;
            }
            ZSTD_outBuffer ob = {out + used, cap - used, 0};
            r = ZSTD_decompressStream(dctx, &ob, &ib); used += ob.pos;
            if (ZSTD_isError(r) || (r && ib.pos == ib.size && ob.pos == 0)) break;
        }
        if (out && r == 0) result = out_array(env, out, used);
        free(out);
    }
    ZSTD_freeDCtx(dctx);
    (*env)->ReleaseByteArrayElements(env, src, in, JNI_ABORT);
    if (!result && !(*env)->ExceptionCheck(env)) fail(env, "zstd frame does not decompress");
    return result;
}

/* Compressed with the registered dictionary of that id (0: none). */
JNIEXPORT jbyteArray JNICALL Java_dev_tok_totk_Zstd_compress(JNIEnv *env, jclass cls, jbyteArray src, jint dictionary) {
    (void)cls;
    const ZSTD_CDict *dict = NULL;
    for (unsigned i = 0; i < ndicts; i++) if (dict_ids[i] == (unsigned)dictionary) dict = cdicts[i];
    if (dictionary && !dict) { fail(env, "no such zstd dictionary"); return NULL; }
    jsize n = (*env)->GetArrayLength(env, src);
    jbyte *in = (*env)->GetByteArrayElements(env, src, NULL);
    size_t cap = ZSTD_compressBound((size_t)n); void *out = malloc(cap);
    ZSTD_CCtx *cctx = ZSTD_createCCtx(); size_t got = 0;
    if (out && cctx) got = dict ? ZSTD_compress_usingCDict(cctx, out, cap, in, (size_t)n, dict) : ZSTD_compressCCtx(cctx, out, cap, in, (size_t)n, LEVEL);
    jbyteArray result = out && cctx && !ZSTD_isError(got) ? out_array(env, out, got) : NULL;
    ZSTD_freeCCtx(cctx); free(out);
    (*env)->ReleaseByteArrayElements(env, src, in, JNI_ABORT);
    if (!result) fail(env, "zstd compression failed");
    return result;
}

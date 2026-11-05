// libtotklsfg.so: host/lsfg_translate.cpp for the startup screen (Lsfg.java), on its background thread. The game's process
// never loads this library.
#include <jni.h>
#include "lsfg_translate.h"

namespace {
struct Progress { JNIEnv *env; jobject target; jmethodID at; };
void on_progress(void *user, int done, int total) {
    auto *p = static_cast<Progress *>(user);
    p->env->CallVoidMethod(p->target, p->at, done, total);
    // the translation goes on: only the display missed a step
    if (p->env->ExceptionCheck()) p->env->ExceptionClear();
}
}

// null, or what went wrong in words for the player
extern "C" JNIEXPORT jstring JNICALL Java_dev_tok_totk_Lsfg_translate(JNIEnv *env, jclass, jstring dll, jstring out_dir, jobject progress) {
    Progress p = {env, progress, env->GetMethodID(env->GetObjectClass(progress), "at", "(II)V")};
    // NoSuchMethodError is pending
    if (!p.at) return nullptr;
    const char *from = env->GetStringUTFChars(dll, nullptr), *to = env->GetStringUTFChars(out_dir, nullptr);
    std::string error = from && to ? lsfg_translate(from, to, on_progress, &p) : "The file could not be opened.";
    if (from) env->ReleaseStringUTFChars(dll, from);
    if (to) env->ReleaseStringUTFChars(out_dir, to);
    return error.empty() ? nullptr : env->NewStringUTF(error.c_str());
}

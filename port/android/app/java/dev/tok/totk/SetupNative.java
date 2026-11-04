package dev.tok.totk;

/** libtotksetup.so (host/setup_jni.c): the container reader, on the startup screen's Game files page. The library is loaded
    here, when that page first needs it, and never in the game's process, whose libtotk.so links the reader itself. Text
    returned names a key, never its value. */
final class SetupNative {
    private static boolean loaded;

    private SetupNative() {}

    static synchronized boolean load() {
        if (!loaded) try { System.loadLibrary("totksetup"); loaded = true; } catch (UnsatisfiedLinkError e) { return false; }
        return true;
    }

    /** null, or why these bytes are not a usable prod.keys. */
    static native String keysError(byte[] text, int size);
    /** tokc_inspect: null with out = {0, title, version, type, is_xci, metas}, or the reader's text with out[0] its code. */
    static native String inspect(String keys, int fd, long[] out);
    /** tokc_game_open: null with out[0] = 0, or the reader's text with out[0] its code. */
    static native String pair(String keys, int base, int update, long[] out);
}

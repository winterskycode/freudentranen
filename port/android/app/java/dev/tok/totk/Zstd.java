package dev.tok.totk;

import java.io.IOException;

/** zstd through libtotkmods.so (host/mods_jni.c), for the mod installer. Register the game's dictionaries
    (Pack/ZsDic.pack.zs) once; decompress picks the one a frame names, compress uses the id given (0: none). */
final class Zstd {
    static { System.loadLibrary("totkmods"); }
    private Zstd() {}
    static native void dictionaries(byte[][] dicts);
    static native int dictionaryOf(byte[] frame);
    static native byte[] decompress(byte[] frame) throws IOException;
    static native byte[] compress(byte[] data, int dictionary) throws IOException;
}

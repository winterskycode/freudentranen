package dev.tok.totk;

import java.io.IOException;

/** The player's own game files for the mod installer, in place of romfs.bin (host/player_romfs_jni.c, in libtotkmods). */
final class PlayerRomfs {
    static { System.loadLibrary("totkmods"); }
    private PlayerRomfs() {}
    /** The descriptors stay open for the life of the process. */
    static native void open(String keysPath, int baseFd, int updateFd) throws IOException;
    static native boolean has(String path);
    static native byte[] read(String path) throws IOException;
    static native String sha(String path) throws IOException;
    /** mod-base.bin at out, from the two packages. */
    static native void makeModBase(String out) throws IOException;
}

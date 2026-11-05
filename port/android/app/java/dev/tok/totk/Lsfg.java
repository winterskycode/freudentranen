package dev.tok.totk;

import java.io.File;
import java.io.IOException;

/** Lossless Scaling's frame-generation shaders, made on the device from the player's own Lossless.dll (Tools > Lossless
    Scaling). libtotklsfg.so (host/lsfg_translate.cpp) translates them; the game reads files/lsfg/<name>.spv
    (vk/lsfg_bridge.cpp). The library is loaded here, on the first translation, and never in the game's process. */
final class Lsfg {
    interface Progress { void at(int done, int total); }

    /** LSFG_SHADER_COUNT in host/lsfg_translate.h. */
    static final int SHADERS = 52;
    /** No Lossless.dll comes near this: a bigger file is not copied to the end. */
    static final long MAX_SIZE = 256L << 20;
    static final String NOT_DLL = "This file is not Lossless.dll.";

    private Lsfg() {}

    /** Are all the shaders there? The settings offer LSFG only then. */
    static boolean ready(File files) {
        String[] names = new File(files, "lsfg").list(); int n = 0;
        if (names != null) for (String s : names) if (s.endsWith(".spv")) n++;
        return n >= SHADERS;
    }

    /** Makes the shaders in a new folder and swaps it in, so a file that fails leaves the shaders already there as they
        are. The IOException's message is for the player. For a background thread. */
    static void install(File dll, File files, Progress progress) throws IOException {
        File dir = new File(files, "lsfg"), fresh = new File(files, "lsfg.new"), old = new File(files, "lsfg.old");
        delete(fresh); delete(old);
        if (!fresh.mkdirs()) throw new IOException("The shaders could not be saved. The device may be out of space.");
        try {
            try { System.loadLibrary("totklsfg"); } catch (UnsatisfiedLinkError e) { throw new IOException("This build of the app cannot translate shaders."); }
            String error = translate(dll.getPath(), fresh.getPath(), progress);
            if (error != null) throw new IOException(error);
            if (dir.exists() && !dir.renameTo(old)) throw new IOException("The shaders could not be saved.");
            if (!fresh.renameTo(dir)) { old.renameTo(dir); throw new IOException("The shaders could not be saved."); }
            delete(old);
        } finally { delete(fresh); }
    }

    /** null, or what went wrong in words for the player. */
    private static native String translate(String dll, String dir, Progress progress);

    private static void delete(File f) {
        File[] kids = f.listFiles(); if (kids != null) for (File k : kids) delete(k);
        f.delete();
    }
}

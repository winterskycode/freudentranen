package android.content;

import java.io.File;

/** What Mods.java asks of a Context, on a PC. */
public class Context {
    private final File files;
    public Context(File files) { this.files = files; }
    public File getFilesDir() { return files; }
}

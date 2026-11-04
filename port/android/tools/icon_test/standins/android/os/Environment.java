package android.os;

/** A player's app: no all-files access. */
public final class Environment {
    private Environment() {}
    public static boolean isExternalStorageManager() { return false; }
}

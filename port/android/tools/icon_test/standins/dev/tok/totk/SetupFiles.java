package dev.tok.totk;

import android.content.Context;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.IOException;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.List;

/** On a PC there are no picks: the real PlayerRomfs opens the files IconTest names, by descriptor as on a device. */
final class SetupFiles {
    private SetupFiles() {}
    static String keys, base, update;
    private static final List<FileInputStream> open = new ArrayList<>();   // the descriptors stay open, as PlayerRomfs asks

    private static int descriptor(String path) throws IOException {
        FileInputStream in = new FileInputStream(path); open.add(in);
        try {
            Field n = FileDescriptor.class.getDeclaredField("fd"); n.setAccessible(true);
            return n.getInt(in.getFD());
        } catch (ReflectiveOperationException e) { throw new IOException(e); }
    }

    static boolean openForMods(Context c) throws IOException {
        PlayerRomfs.open(keys, descriptor(base), descriptor(update));
        return true;
    }
}

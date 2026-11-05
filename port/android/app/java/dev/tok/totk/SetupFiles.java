package dev.tok.totk;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.CancellationSignal;
import android.os.OperationCanceledException;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.system.ErrnoException;
import android.system.Os;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

/** The Android side of Setup.java: documents, their descriptors and the permissions that keep them readable. */
final class SetupFiles implements Setup.Reader, Setup.Tree {
    private static final String[] COLUMNS = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
        DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE, DocumentsContract.Document.COLUMN_FLAGS};
    private static final int GRANT = Intent.FLAG_GRANT_READ_URI_PERMISSION;

    final Setup state;
    private final ContentResolver resolver;
    private volatile Uri tree;
    private volatile CancellationSignal signal;

    SetupFiles(Context c) { resolver = c.getContentResolver(); state = new Setup(c.getNoBackupFilesDir(), this); }

    /** Stops a folder's walk, and a provider slow to open a file if it listens. */
    void cancel() { CancellationSignal s = signal; if (s != null) s.cancel(); }
    private boolean stopped() { CancellationSignal s = signal; return s != null && s.isCanceled(); }

    // ---- the picks (background threads) ----

    void takeKeys(Uri uri) {
        try (InputStream in = resolver.openInputStream(uri)) {
            if (in == null) throw new IOException();
            state.takeKeys(in);
        } catch (IOException | RuntimeException e) { state.keysUnreadable(); }
    }

    /** The name and size the provider gives for a file picked on its own. */
    private Setup.Doc doc(Uri uri) {
        String name = null; long size = -1;
        // a failed query leaves the name and size unknown: the reader finds out whether it opens
        try (Cursor q = resolver.query(uri, new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE}, null, null, null)) {
            if (q != null && q.moveToFirst()) { name = q.getString(0); if (!q.isNull(1)) size = q.getLong(1); }
        } catch (RuntimeException e) { }
        return new Setup.Doc(uri.toString(), uri.toString(), name, size);
    }

    void takeFiles(List<Uri> uris, int slot) {
        List<Setup.Doc> docs = new ArrayList<>();
        for (Uri u : uris) docs.add(doc(u));
        state.takeFiles(docs, slot);
    }

    void takeFolder(Uri picked, Setup.Progress progress) {
        tree = picked; signal = new CancellationSignal();
        try { state.takeFolder(this, DocumentsContract.getTreeDocumentId(picked), progress, this::stopped); }
        finally { signal = null; }
    }

    // ---- Setup.Tree ----

    @Override public List<Setup.Entry> children(String id) throws IOException {
        List<Setup.Entry> out = new ArrayList<>();
        try (Cursor c = resolver.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, id), COLUMNS, null, null, null)) {
            if (c == null) throw new IOException();
            while (c.moveToNext()) {
                // has no bytes of its own
                if ((c.getInt(4) & DocumentsContract.Document.FLAG_VIRTUAL_DOCUMENT) != 0) continue;
                out.add(new Setup.Entry(c.getString(0), c.getString(1), DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2)), c.isNull(3) ? -1 : c.getLong(3)));
            }
        }
        return out;
    }

    @Override public Setup.Doc doc(Setup.Entry e) {
        return new Setup.Doc(DocumentsContract.buildDocumentUriUsingTree(tree, e.id).toString(), tree.toString(), e.name, e.size);
    }

    // ---- Setup.Reader ----

    private ParcelFileDescriptor open(Setup.Doc d) throws IOException {
        ParcelFileDescriptor p = resolver.openFileDescriptor(Uri.parse(d.uri), "r", signal);
        if (p == null) throw new IOException();
        return p;
    }

    /** A pipe or socket from a provider that streams: the reader needs a file it can read at any offset. */
    private static boolean regular(ParcelFileDescriptor p) { return p.getStatSize() >= 0; }

    @Override public String keysError(byte[] text, int size) {
        return SetupNative.load() ? SetupNative.keysError(text, size) : Setup.NO_READER;
    }

    @Override public Setup.Info inspect(Setup.Doc d, File keys) {
        if (!SetupNative.load()) return new Setup.Info(Setup.E_NO_READER, null);
        try (ParcelFileDescriptor p = open(d)) {
            if (!regular(p)) return new Setup.Info(Setup.E_NOT_FILE, null);
            long[] o = new long[6];
            String text = SetupNative.inspect(keys.getPath(), p.getFd(), o);
            return text != null ? new Setup.Info((int) o[0], text) : new Setup.Info(o[1], (int) o[2], (int) o[3], o[4] != 0, (int) o[5]);
        } catch (IOException | OperationCanceledException | SecurityException | IllegalArgumentException | IllegalStateException e) { return new Setup.Info(Setup.E_NOT_OPENED, null); }
    }

    @Override public Setup.Info pair(Setup.Doc base, Setup.Doc update, File keys) {
        if (!SetupNative.load()) return new Setup.Info(Setup.E_NO_READER, null);
        try (ParcelFileDescriptor b = open(base); ParcelFileDescriptor u = open(update)) {
            if (!regular(b) || !regular(u)) return new Setup.Info(Setup.E_NOT_FILE, null);
            long[] o = new long[1];
            String text = SetupNative.pair(keys.getPath(), b.getFd(), u.getFd(), o);
            return text != null ? new Setup.Info((int) o[0], text) : new Setup.Info(0L, 0, 0, false, 0);
        } catch (IOException | OperationCanceledException | SecurityException | IllegalArgumentException | IllegalStateException e) { return new Setup.Info(Setup.E_NOT_OPENED, null); }
    }

    @Override public boolean keep(Setup.Doc d) {
        try { resolver.takePersistableUriPermission(Uri.parse(d.grant), GRANT); return true; }
        catch (RuntimeException e) { return false; }
    }

    @Override public void drop(String grant) {
        // a grant that cannot be released went with the file
        try { resolver.releasePersistableUriPermission(Uri.parse(grant), GRANT); } catch (RuntimeException e) { }
    }

    // ---- GameActivity: the process :game ----

    /** The file's descriptor for libtotk.so, which keeps it for the life of the process. -1: reported to the launcher. */
    private static int forGame(ContentResolver r, Setup s, int slot) {
        Setup.Doc d = s.doc(slot);
        try {
            ParcelFileDescriptor p = r.openFileDescriptor(Uri.parse(d.uri), "r");
            if (p == null) throw new IOException();
            return p.detachFd();
        } catch (IOException | RuntimeException e) { s.report(slot); return -1; }
    }

    /** The mod installer's way in, in the launcher's process. False: no picks, it uses the prepared data. */
    static boolean openForMods(Context c) throws IOException {
        ContentResolver r = c.getContentResolver();
        Setup s = new Setup(c.getNoBackupFilesDir(), null);
        if (!s.complete()) return false;
        int base = forGame(r, s, Setup.BASE), update = base < 0 ? -1 : forGame(r, s, Setup.UPDATE);
        if (update < 0) throw new IOException("The game files cannot be read where they are. Pick them again under Tools.");
        PlayerRomfs.open(s.keysFile().getPath(), base, update);
        return true;
    }

    /** Before libtotk.so loads: the picks as TOTK_BASE_FD, TOTK_UPDATE_FD and TOTK_KEYS (a path, never the keys). With no picks
        nothing is set, and the game uses the prepared data under /sdcard/tok. False: a pick is gone, the launcher will say so
        and TOTK_SETUP_STOP keeps the library from starting the game. */
    static boolean openForGame(Context c) {
        ContentResolver r = c.getContentResolver();
        Setup s = new Setup(c.getNoBackupFilesDir(), null);
        if (!s.complete()) return true;
        // setenv fails only out of memory
        try {
            int base = forGame(r, s, Setup.BASE), update = base < 0 ? -1 : forGame(r, s, Setup.UPDATE);
            if (update < 0) { Os.setenv("TOTK_SETUP_STOP", "1", true); return false; }
            Os.setenv("TOTK_BASE_FD", Integer.toString(base), true);
            Os.setenv("TOTK_UPDATE_FD", Integer.toString(update), true);
            Os.setenv("TOTK_KEYS", s.keysFile().getPath(), true);
            return true;
        } catch (ErrnoException e) { throw new IllegalStateException(e); }
    }
}

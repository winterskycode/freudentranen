package dev.tok.totk;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileNotFoundException;

/** content://dev.tok.totk.reports/<name>.zip: a bug report's zip (ReportStore.zip) for the share sheet, read-only, to the
    app the user picks (a granted URI permission). Only zips directly under the reports folder are served. */
public class ReportProvider extends ContentProvider {
    static final String AUTHORITY = "dev.tok.totk.reports";

    static Uri uri(File zip) { return Uri.parse("content://" + AUTHORITY + "/" + Uri.encode(zip.getName())); }

    private static File file(Uri uri) throws FileNotFoundException {
        String name = uri.getLastPathSegment();
        if (name == null || name.contains("/") || name.startsWith(".") || !name.endsWith(".zip")) throw new FileNotFoundException(String.valueOf(uri));
        File f = new File(ReportStore.ROOT, name);
        if (!f.isFile()) throw new FileNotFoundException(String.valueOf(uri));
        return f;
    }

    @Override public boolean onCreate() { ReportStore.context(getContext()); return true; }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        try {
            File f = file(uri);
            MatrixCursor c = new MatrixCursor(new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
            c.addRow(new Object[] {f.getName(), f.length()});
            return c;
        } catch (FileNotFoundException e) { return null; }
    }

    @Override public String getType(Uri uri) { return "application/zip"; }
    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String selection, String[] args) { return 0; }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { return 0; }
}

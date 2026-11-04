package dev.tok.totk;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** A folder picked with ACTION_OPEN_DOCUMENT_TREE, as a source of a save (Saves.java). The document providers do not show
    links: they hand out what a link points to, or leave it out, so here a link cannot be refused; the depth and file
    limits stop a loop of them. */
final class SaveDocuments implements Saves.Source {
    private static final String[] COLUMNS = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
        DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};

    private final ContentResolver resolver;
    private final Uri tree;
    private final Map<String, String> ids = new HashMap<>();

    SaveDocuments(ContentResolver resolver, Uri tree) { this.resolver = resolver; this.tree = tree; }

    @Override public List<Saves.Item> list() throws IOException {
        List<Saves.Item> out = new ArrayList<>();
        walk(DocumentsContract.getTreeDocumentId(tree), "", 0, out);
        return out;
    }

    private void walk(String id, String prefix, int depth, List<Saves.Item> out) throws IOException {
        if (depth > Saves.MAX_DEPTH) throw new Saves.Problem(Saves.TOO_MANY);
        Map<String, String> folders = new HashMap<>();
        try (Cursor c = resolver.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, id), COLUMNS, null, null, null)) {
            if (c == null) throw new Saves.Problem(Saves.NOT_READ);
            while (c.moveToNext()) {
                String path = prefix + Saves.safePath(c.getString(1));
                boolean dir = DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2));
                out.add(new Saves.Item(path, dir, dir ? 0 : c.isNull(3) ? -1 : c.getLong(3)));
                if (out.size() > Saves.MAX_FILES) throw new Saves.Problem(Saves.TOO_MANY);
                ids.put(path, c.getString(0));
                if (dir) folders.put(path, c.getString(0));
            }
        }
        for (Map.Entry<String, String> f : folders.entrySet()) walk(f.getValue(), f.getKey() + "/", depth + 1, out);
    }

    @Override public InputStream open(String path) throws IOException {
        return resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, ids.get(path)));
    }
}

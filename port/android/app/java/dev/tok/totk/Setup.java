package dev.tok.totk;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/** The Game files page's decisions (first-run setup, setup/README.md): the player's prod.keys, copied once into private
    storage, and the base game and update, read in place at every start. What each picked file is comes from its content
    (tokc_inspect), never its name. The page shows one line per input, a tick or what to do about it.

    No Android classes: tools/setup_test runs all of this on a PC. SetupFiles.java is the Android side (documents, descriptors,
    permissions) and the library the reader runs in, libtotksetup.so, is reached only through Reader, so the game's process,
    which reads the saved picks here too, never loads it.

    Everything lives in one folder outside the app's files/: the platform's no-backup folder, which cloud backups leave out,
    and which no bug report copies from (host/report.c, ReportStore.crashReport name their files one by one). */
final class Setup {
    // container.h
    static final long TITLE_BASE = 0x0100F2C0115B6000L, TITLE_UPDATE = 0x0100F2C0115B6800L;
    static final int VERSION_BASE = 0, VERSION_UPDATE = 720896, META_APPLICATION = 0x80, META_PATCH = 0x81;
    static final int E_IO = -2, E_FORMAT = -3, E_KEY_MISSING = -5, E_KEY_WRONG = -6, E_TICKET = -7, E_WRONG_GAME = -8, E_WRONG_VERSION = -9, E_SWAPPED = -10;
    /** Not the reader's: the provider gave no file (SetupFiles), or this build has no reader. */
    static final int E_NOT_FILE = -100, E_NOT_OPENED = -101, E_NO_READER = -102;

    static final int BASE = 0, UPDATE = 1;
    // keys.c MAX_KEYS_FILE
    static final int MAX_KEYS = 1 << 20;
    /** A folder's walk: the depth and entries listed, the files opened and for how long. Each file opened costs a header read,
        never the file (the reader reads its tables: at most a few MB of a hostile file, a few KB of any other). */
    static final int MAX_DEPTH = 6, MAX_LISTED = 20000, MAX_OPENED = 200;
    /** Below this a file is not opened in a folder's walk: every version of the game's packages is far larger. */
    static final long MIN_SIZE = 16L << 20;

    static final String KEYS_FIRST = "Pick prod.keys first. The app needs it to read the game files.";
    static final String KEYS_UNREADABLE = "This file could not be read. Pick it again.";
    static final String KEYS_TOO_BIG = "This file is too large to be prod.keys.";
    static final String KEYS_NOT_SAVED = "The keys could not be saved. The device may be out of space.";
    static final String KEYS_WRONG = "These keys do not open the game files. Pick a prod.keys that matches them.";
    static final String OPEN_WRONG = "The keys cannot open this file. It is not a game file, or prod.keys does not match it.";
    static final String OTHER_UPDATE = "This update is another version. The game needs update " + Game.VERSION + ".";
    static final String OTHER_BASE = "This base game is not version 1.0.0.";
    static final String OTHER_GAME = "This file is another game, not " + Game.TITLE + ".";
    static final String NOT_GAME = "This is not an NSP or XCI game file.";
    static final String DAMAGED = "This file is damaged or incomplete. Pick a complete copy.";
    static final String NOT_READ = "This file could not be read. Pick it again.";
    static final String NOT_FILE = "This file cannot be read where it is. Move it to the device's storage or memory card, then pick it again.";
    static final String NOT_OPENED = "This file could not be opened. Pick it again.";
    static final String NO_READER = "This build of the app cannot read game files.";
    static final String NOT_KEPT = "Android does not let the app keep access to this file. Move it to the device's storage, then pick it again.";
    static final String NEEDS_KEYS = "This file needs other keys. See the first row.";
    static final String BOTH_BASE = "Both files are the base game. Pick the " + Game.VERSION + " update.";
    static final String BOTH_UPDATE = "Both files are the update. Pick the base game.";
    static final String NOT_IN_FOLDER = "Not found in that folder.";
    static final String TOO_MANY = "That folder holds too many files to look through. Pick the folder the game is in, or the two files.";
    static final String NOT_LISTED = "That folder could not be read. Pick another, or pick the two files.";
    static final String BASE_GONE = "The base game file cannot be opened. It may have been moved or deleted, or its memory card taken out. Pick it again.";
    static final String UPDATE_GONE = "The update file cannot be opened. It may have been moved or deleted, or its memory card taken out. Pick it again.";

    /** A file: picked, or found in a picked folder. grant is the URI whose permission keeps it readable: its own or the folder's.
        size is -1 when the provider does not say. */
    static final class Doc {
        final String uri, grant, name; final long size;
        Doc(String uri, String grant, String name, long size) { this.uri = uri; this.grant = grant; this.name = clean(name); this.size = size; }
    }

    /** What the reader made of a file or the pair: code 0, or a TOKC_E_ code with the reader's text. */
    static final class Info {
        final int code, version, type, metas; final long title; final boolean xci; final String text;
        Info(long title, int version, int type, boolean xci, int metas) { code = 0; this.title = title; this.version = version; this.type = type; this.xci = xci; this.metas = metas; text = ""; }
        Info(int code, String text) { this.code = code; this.text = text == null ? "" : text; title = 0; version = type = metas = 0; xci = false; }
    }

    /** An entry of a picked folder. */
    static final class Entry {
        final String id, name; final boolean folder; final long size;
        Entry(String id, String name, boolean folder, long size) { this.id = id; this.name = name; this.folder = folder; this.size = size; }
    }

    interface Reader {
        /** null, or the reader's text: size bytes of text, read as prod.keys. */
        String keysError(byte[] text, int size);
        Info inspect(Doc d, File keys);
        /** Opens the pair as the game does: code 0 when it can be played. */
        Info pair(Doc base, Doc update, File keys);
        /** Keeps the file readable after a restart; false when the provider refuses. */
        boolean keep(Doc d);
        void drop(String grant);
    }
    interface Tree {
        List<Entry> children(String id) throws IOException;
        Doc doc(Entry e);
    }
    interface Progress { void at(String what); }
    interface Stop { boolean stopped(); }

    private final File dir, keys, state, reported;
    private final Reader reader;
    private volatile Doc base, update;
    // the pair opened as the game opens it
    private volatile boolean checked;
    private volatile String keysProblem, baseProblem, updateProblem, pairWhy;
    // files the keys could not read: looked at again with new keys
    private volatile List<Doc> retry = new ArrayList<>();
    long scanLimitMs = 30000;

    /** reader: null in the game's process, which only reads what was saved. */
    Setup(File dir, Reader reader) {
        this.dir = dir; this.reader = reader;
        keys = new File(dir, "prod.keys"); state = new File(dir, "game-files.txt"); reported = new File(dir, "game-files-problem.txt");
        load();
        // A check that failed left no words behind, and a file that could not be read may be back.
        if (reader != null && !checked) recheck();
    }

    File keysFile() { return keys; }
    Doc doc(int slot) { return slot == BASE ? base : update; }
    boolean hasKeys() { return keys.isFile(); }
    /** The picks stand: the game starts from them. */
    boolean complete() { return checked && base != null && update != null && hasKeys(); }
    String keysProblem() { return keysProblem; }
    String problem(int slot) { return slot == BASE ? baseProblem : updateProblem; }
    boolean keysOk() { return hasKeys() && keysProblem == null; }
    boolean ok(int slot) { return doc(slot) != null && problem(slot) == null; }
    void askForKeys() { keysProblem = KEYS_FIRST; }
    void keysUnreadable() { refuseKeys(KEYS_UNREADABLE); }

    private void set(int slot, String why) { if (slot == BASE) baseProblem = why; else updateProblem = why; }
    private static String clean(String s) { return s == null ? "file" : s.replaceAll("[\\t\\r\\n]", " "); }

    // ---- the keys ----

    private static String sentence(String s) { return s.isEmpty() ? s : s.substring(0, 1).toUpperCase(Locale.ROOT) + s.substring(1); }

    private void refuseKeys(String why) { keysProblem = hasKeys() ? why + " Your earlier keys are still in use." : why; }

    /** prod.keys from the picker: checked in memory, and copied over the private one only if good, so a bad file changes
        nothing. Text never carries a key's value: the reader's names a key, and that is all that is shown of this file. */
    void takeKeys(InputStream in) {
        byte[] text = new byte[MAX_KEYS + 1]; int n = 0;
        try {
            for (int r; n < text.length && (r = in.read(text, n, text.length - n)) > 0; ) n += r;
            if (n > MAX_KEYS) { refuseKeys(KEYS_TOO_BIG); return; }
            String why = reader.keysError(text, n);
            if (why != null) { refuseKeys("This file cannot be used. " + (why.startsWith("prod.keys") ? why : sentence(why)) + "."); return; }
            if (!write(keys, text, n)) { refuseKeys(KEYS_NOT_SAVED); return; }
        } catch (IOException | RuntimeException e) { refuseKeys(KEYS_UNREADABLE); return; }
        finally { Arrays.fill(text, (byte) 0); }
        keysProblem = null;
        List<Doc> again = retry; retry = new ArrayList<>();
        if (!again.isEmpty()) { baseProblem = updateProblem = null; sort(again, false, BASE, false, what -> { }, () -> false); }
        else recheck();
        save();
    }

    /** Mode 600 from the start, then renamed over the old one: a failure leaves the old one whole. */
    private boolean write(File to, byte[] data, int size) {
        Path tmp = new File(to.getPath() + ".new").toPath();
        try {
            dir.mkdirs();
            Files.deleteIfExists(tmp);
            Files.createFile(tmp, PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
            try (OutputStream out = Files.newOutputStream(tmp)) { out.write(data, 0, size); }
            Files.move(tmp, to.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
            return true;
        } catch (IOException | RuntimeException e) {
            // a leftover temporary file is replaced by the next write
            try { Files.deleteIfExists(tmp); } catch (IOException | RuntimeException ignored) { }
            return false;
        }
    }

    // ---- the game files ----

    private enum Kind { BASE, UPDATE, OTHER_UPDATE, OTHER_BASE, OTHER_GAME, BAD }

    static boolean isUpdate(Info i) { return i.title == TITLE_UPDATE && i.type == META_PATCH; }
    static boolean isBase(Info i) { return i.title == TITLE_BASE && i.type == META_APPLICATION; }

    private static Kind kind(Info i) {
        if (i.code != 0) return Kind.BAD;
        if (isBase(i)) return i.version == VERSION_BASE ? Kind.BASE : Kind.OTHER_BASE;
        if (isUpdate(i)) return i.version == VERSION_UPDATE ? Kind.UPDATE : Kind.OTHER_UPDATE;
        return Kind.OTHER_GAME;
    }

    /** The reader's words where they are the player's words too; its internal terms mean nothing here. */
    static String plain(Info i) {
        switch (i.code) {
            case E_KEY_MISSING: {
                String name = i.text.replace("prod.keys has no ", "").trim();
                return "Your prod.keys lacks " + (name.matches("[a-z0-9_]+") ? name : "a key") + ", which this game needs. Pick a prod.keys that has it.";
            }
            case E_KEY_WRONG: return i.text.startsWith("no NCA") ? OPEN_WRONG : KEYS_WRONG;
            case E_IO: return NOT_READ;
            case E_WRONG_GAME: return OTHER_GAME;
            case E_WRONG_VERSION: return i.text.contains("update") ? OTHER_UPDATE : OTHER_BASE;
            case E_SWAPPED: return i.text.contains("is the update") ? BOTH_UPDATE : BOTH_BASE;
            case E_NOT_FILE: return NOT_FILE;
            case E_NOT_OPENED: return NOT_OPENED;
            case E_NO_READER: return NO_READER;
            default:
                if (i.text.startsWith("this is not an NSP")) return NOT_GAME;
                if (i.text.isEmpty() || i.text.matches(".*(NCA|PFS0|HFS0|IVFC|ExeFS|RomFS|BKTR|hash|table|section|meta|out of bounds).*")) return DAMAGED;
                return sentence(i.text) + ".";
        }
    }

    private void put(int slot, Doc d) {
        Doc old = doc(slot), other = doc(1 - slot);
        if (!reader.keep(d)) { set(slot, NOT_KEPT); return; }
        if (slot == BASE) base = d; else update = d;
        set(slot, null);
        if (old != null && !old.grant.equals(d.grant) && (other == null || !old.grant.equals(other.grant))) reader.drop(old.grant);
    }

    /** A rejected file leaves the pick it would have replaced in place, and says so. */
    private void refuse(int slot, String why) {
        Doc d = doc(slot);
        set(slot, d != null ? why + " Still using " + d.name + "." : why);
    }

    /** Files from the picker, one or both: each goes where its content says. slot: the row that was pressed, which is told about
        a file that is neither. */
    void takeFiles(List<Doc> docs, int slot) {
        if (!hasKeys()) { askForKeys(); return; }
        sort(docs, false, slot, false, what -> { }, () -> false);
        save();
    }

    /** Everything worth opening in a picked folder, the largest first: the game's two files are the first ones met in most
        folders, and a walk that stops early has seen the likeliest ones. A loop of links ends at MAX_DEPTH. */
    static List<Doc> candidates(Tree tree, String root, Stop stop, boolean[] cut) throws IOException {
        List<Doc> out = new ArrayList<>();
        List<String> level = new ArrayList<>(); level.add(root);
        Set<String> seen = new HashSet<>();
        int listed = 0;
        for (int depth = 0; !level.isEmpty() && !stop.stopped(); depth++) {
            if (depth > MAX_DEPTH) { cut[0] = true; break; }
            List<String> next = new ArrayList<>();
            for (String id : level) {
                if (!seen.add(id)) continue;
                List<Entry> entries;
                try { entries = tree.children(id); }
                // a subfolder that cannot be read is skipped
                catch (IOException | RuntimeException e) { if (depth == 0) throw new IOException(e); continue; }
                for (Entry e : entries) {
                    if (++listed > MAX_LISTED) { cut[0] = true; next.clear(); break; }
                    if (e.folder) next.add(e.id);
                    else if (e.size >= MIN_SIZE || e.size < 0) out.add(tree.doc(e));
                }
                if (cut[0]) break;
            }
            level = next;
        }
        out.sort((a, b) -> Long.compare(b.size, a.size));
        return out;
    }

    /** A picked folder: both files are looked for inside it, in any subfolder. */
    void takeFolder(Tree tree, String root, Progress progress, Stop stop) {
        if (!hasKeys()) { askForKeys(); return; }
        boolean[] cut = {false};
        List<Doc> docs;
        progress.at("Looking through the folder…");
        try { docs = candidates(tree, root, stop, cut); }
        catch (IOException | RuntimeException e) { refuse(BASE, NOT_LISTED); refuse(UPDATE, NOT_LISTED); save(); return; }
        if (!stop.stopped()) sort(docs, true, BASE, cut[0], progress, stop);
        save();
    }

    private void sort(List<Doc> docs, boolean folder, int slot, boolean cut, Progress progress, Stop stop) {
        Doc newBase = null, newUpdate = null;
        int bases = 0, updates = 0, opened = 0;
        String baseWhy = null, updateWhy = null, keysWhy = null;
        boolean otherGame = false;
        List<String> others = new ArrayList<>(); List<Doc> locked = new ArrayList<>();
        long start = System.nanoTime();
        for (Doc d : docs) {
            if (stop.stopped()) return;
            if (folder && (opened >= MAX_OPENED || (System.nanoTime() - start) / 1000000 > scanLimitMs)) { cut = true; break; }
            opened++;
            if (folder) progress.at("Looking through the folder… file " + opened + " of " + docs.size());
            Info i = reader.inspect(d, keys);
            switch (kind(i)) {
                case BASE: bases++; if (newBase == null) newBase = d; break;
                case UPDATE: updates++; if (newUpdate == null) newUpdate = d; break;
                case OTHER_UPDATE: updateWhy = OTHER_UPDATE; break;
                case OTHER_BASE: baseWhy = OTHER_BASE; break;
                case OTHER_GAME: otherGame = true; others.add(OTHER_GAME); break;
                default:
                    if (i.code == E_KEY_MISSING) { keysWhy = plain(i); locked.add(d); } else others.add(plain(i));
            }
            if (folder && newBase != null && newUpdate != null) break;
        }
        if (stop.stopped()) return;
        retry = new ArrayList<>(); keysProblem = null;
        if (newBase != null) put(BASE, newBase);
        if (newUpdate != null) put(UPDATE, newUpdate);
        if (keysWhy != null && (!folder || newBase == null || newUpdate == null)) { keysProblem = keysWhy; retry = locked; }
        if (folder) {
            String none = cut ? TOO_MANY : keysWhy != null ? NEEDS_KEYS : otherGame ? OTHER_GAME : others.isEmpty() ? NOT_IN_FOLDER : NOT_IN_FOLDER + " " + others.get(0);
            if (newBase == null) refuse(BASE, baseWhy != null ? baseWhy : none);
            if (newUpdate == null) refuse(UPDATE, updateWhy != null ? updateWhy : none);
        } else {
            if (baseWhy != null) refuse(BASE, baseWhy);
            if (updateWhy != null) refuse(UPDATE, updateWhy);
            if (bases > 1 && update == null) refuse(UPDATE, BOTH_BASE);
            if (updates > 1 && base == null) refuse(BASE, BOTH_UPDATE);
            if (keysWhy != null) others.add(NEEDS_KEYS);
            // the row pressed, unless a file just filled it
            int row = newBase != null && slot == BASE || newUpdate != null && slot == UPDATE ? 1 - slot : slot;
            for (String why : others) { refuse(row, why); row = 1 - row; }
        }
        recheck();
    }

    /** Both files as the game opens them: the only check of what they hold together, such as the keys a section needs. */
    private void recheck() {
        checked = false;
        if (pairWhy != null) { if (pairWhy.equals(baseProblem)) baseProblem = null; if (pairWhy.equals(updateProblem)) updateProblem = null; pairWhy = null; }
        Doc b = base, u = update;
        if (b == null || u == null || !hasKeys() || keysProblem != null) return;
        Info r = reader.pair(b, u, keys);
        if (r.code == 0) { checked = true; return; }
        String why = plain(r);
        if (r.code == E_KEY_MISSING || r.code == E_KEY_WRONG) keysProblem = why;
        else {
            pairWhy = why;
            // the reader does not say which
            if (r.code == E_TICKET) baseProblem = updateProblem = why;
            else set(r.text.contains("update") ? UPDATE : BASE, why);
        }
    }

    // ---- what is kept: the picks, in a file (SharedPreferences are cached per process, and the game's process reads this) ----

    private static String line(String name, Doc d) { return name + "\t" + d.size + "\t" + d.uri + "\t" + d.grant + "\t" + d.name + "\n"; }

    private void save() {
        Doc b = base, u = update;
        StringBuilder s = new StringBuilder("v1\n");
        if (checked) s.append("checked\n");
        if (b != null) s.append(line("base", b));
        if (u != null) s.append(line("update", u));
        byte[] bytes = s.toString().getBytes(StandardCharsets.UTF_8);
        write(state, bytes, bytes.length);
    }

    /** A file that cannot be understood is the same as no picks: the page asks again. */
    private void load() {
        List<String> lines;
        try { lines = Files.readAllLines(state.toPath(), StandardCharsets.UTF_8); }
        catch (IOException | RuntimeException e) { return; }
        if (lines.isEmpty() || !lines.get(0).equals("v1")) return;
        for (String l : lines) {
            String[] f = l.split("\t", 5);
            if (l.equals("checked")) checked = true;
            else if (f.length == 5 && (f[0].equals("base") || f[0].equals("update"))) {
                Doc d;
                try { d = new Doc(f[2], f[3], f[4], Long.parseLong(f[1])); } catch (NumberFormatException e) { continue; }
                if (f[0].equals("base")) base = d; else update = d;
            }
        }
    }

    // ---- GameActivity found a file gone ----

    /** The game's process tells the launcher, which it cannot reach any other way once it has finished. */
    void report(int slot) {
        byte[] bytes = (slot + "\t" + (slot == BASE ? BASE_GONE : UPDATE_GONE)).getBytes(StandardCharsets.UTF_8);
        write(reported, bytes, bytes.length);
    }

    /** What report wrote, put on its row. The pick stands: a memory card put back in makes it good again. */
    boolean takeReported() {
        if (!reported.exists()) return false;
        try {
            String[] f = new String(Files.readAllBytes(reported.toPath()), StandardCharsets.UTF_8).split("\t", 2);
            if (f.length == 2 && (f[0].equals("0") || f[0].equals("1"))) set(Integer.parseInt(f[0]), f[1]);
        // the page still opens, and the next start tells again
        } catch (IOException | RuntimeException e) { }
        reported.delete();
        return true;
    }
}

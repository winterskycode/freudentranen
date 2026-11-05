package dev.tok.totk;

import java.io.Closeable;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.SecureRandom;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Date;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipException;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/** The game's save: import, export and backups, as tools/saves.py does them over adb (Tools > Saves). No Android classes,
    so tools/saves_test runs it on a desktop JVM; LauncherActivity supplies the pickers and SaveDocuments a picked folder.

    The save is files/profile/save/<account>/: the folder the game names from its user in force (runtime/account.c,
    fs_host.c). A save is what save_manager.py's validate_save accepts: at least one slot_00 ... slot_05 folder with a
    non-empty progress.sav and caption.sav, no incomplete slot, no links or special files. Its files are copied unchanged.

    Replacing it: the new save is unpacked into files/save-staging and checked; the current one is copied to
    files/save-backups/<time>-profile (the folder saves.py uses) and checked; the new one is copied to <account>.new,
    checked by SHA-256, and swapped in by two renames. Java has no atomic exchange, so recover() settles a swap that was cut
    between them (<account> gone, <account>.old there): the launcher runs it before anything can start the game.

    A profile the game has never opened has no account yet. A folder in save/ named with a fresh id is then enough: with no
    users.txt and no account.uid the game takes its users back from those folders (users_from_saves), the first one as
    "Local", in force. Nothing else about the account is written here.

    Every Problem's message is for the player. Methods that touch files are for a background thread. */
class Saves {
    static final int KEEP = 10, MAX_FILES = 20000, MAX_DEPTH = 16;
    /** A save is tens of MB; this leaves room for a full album. */
    static final long MAX_BYTES = 1L << 30;

    static final String RUNNING = "The game is running. Quit it first, then try again. If you have just quit, wait a few seconds.";
    static final String NOT_A_SAVE = "This is not a save. A save has folders named slot_00 to slot_05, with a saved game in at least one.";
    static final String TWO_SAVES = "This holds more than one save. Choose a zip or folder with only one.";
    static final String DAMAGED = "This zip is damaged or incomplete. Copy it to this device again.";
    static final String TOO_BIG = "This is too big to be a save.";
    static final String TOO_MANY = "This has too many files to be a save.";
    static final String LINK = "This holds a link or a special file, which a save never has.";
    static final String NO_SAVE = "There is no save to export yet. Play until the game has saved.";
    static final String NO_SPACE = "There is not enough free space on this device.";
    static final String NOT_COPIED = "The copy did not match, so your save was not changed. Try again.";
    static final String NOT_READ = "This could not be read, so your save was not changed.";
    static final String START_ONCE = "Start the game once and quit it, then try again.";
    static final String BAD_ACCOUNT = "The game's account file is damaged, so there is no folder to put a save in.";
    static final String NO_BACKUP = "That backup is no longer there.";

    private static final Pattern SWAP = Pattern.compile("([0-9a-f]{32})\\.(new|old)"), ACCOUNT = Pattern.compile("[0-9a-f]{32}");
    private static final Pattern SLOT = Pattern.compile("slot_0[0-5]");
    /** Static: an activity recreated in mid-task must see it. */
    private static final AtomicBoolean BUSY = new AtomicBoolean();

    /** A failure with words for the player. */
    static final class Problem extends IOException {
        private static final long serialVersionUID = 1;
        Problem(String message) { super(message); }
    }

    interface Progress { void at(String what); }
    interface Guard { boolean gameRunning(); }

    /** One file or folder of a tree; path is relative, with / between names, and size is -1 when not known yet. */
    static final class Item {
        final String path; final boolean dir; final long size;
        Item(String path, boolean dir, long size) { this.path = path; this.dir = dir; this.size = size; }
    }

    /** Somewhere a save is read from: a zip, a picked folder, a backup. */
    interface Source {
        List<Item> list() throws IOException;
        InputStream open(String path) throws IOException;
    }

    /** The folder the save goes in; fresh: an id made here, the game has no account yet. */
    static final class Account {
        final String folder, name; final int users; final boolean fresh;
        Account(String folder, String name, int users, boolean fresh) { this.folder = folder; this.name = name; this.users = users; this.fresh = fresh; }
    }

    /** What a save holds, for the screen. */
    static final class Info {
        final List<String> slots; final long bytes; final int files;
        Info(List<Item> items) {
            slots = slots(items); long b = 0; int n = 0;
            for (Item i : items) if (!i.dir) { b += i.size; n++; }
            bytes = b; files = n;
        }
    }

    static final class Backup {
        final String name; final Info info;
        Backup(String name, Info info) { this.name = name; this.info = info; }
    }

    /** A save unpacked and checked in files/save-staging, nothing changed yet. */
    static final class Staged {
        final File root; final Map<String, String> sums; final Info info;
        Staged(File root, Map<String, String> sums, Info info) { this.root = root; this.sums = sums; this.info = info; }
    }

    private final File files;
    private final Guard guard;

    Saves(File files, Guard guard) { this.files = files; this.guard = guard; }

    /** One task at a time. The launcher takes this on the UI thread, before starting the task, and refuses to start the game while it is held. */
    static boolean begin() { return BUSY.compareAndSet(false, true); }
    static void end() { BUSY.set(false); }
    static boolean busy() { return BUSY.get(); }

    /** tools/saves_test stops the process at these points. */
    void checkpoint(String where) { }
    long now() { return System.currentTimeMillis(); }
    long free() { return files.getUsableSpace(); }

    private File saveDir() { return new File(files, "profile/save"); }
    private File backupDir() { return new File(files, "save-backups"); }
    private File staging() { return new File(files, "save-staging"); }

    // ---- the account ----

    /** The save folder of the user the game opens with, read as runtime/account.c reads it: users.txt, else account.uid, else
        the save folders, else a new id. */
    Account account() throws IOException {
        File profile = new File(files, "profile"), store = new File(profile, "users.txt"), uid = new File(profile, "account.uid");
        List<String> ids = new ArrayList<>(), names = new ArrayList<>(); int marked = -1;
        if (store.isFile())
            for (String line : new String(Files.readAllBytes(store.toPath()), StandardCharsets.UTF_8).split("\n")) {
                String l = line.endsWith("\r") ? line.substring(0, line.length() - 1) : line;
                boolean star = l.startsWith("*"); if (star) l = l.substring(1);
                // 8 is RUNTIME_USERS_MAX
                int tab = l.indexOf('\t'); if (tab < 0 || ids.size() == 8) continue;
                // the folder's name is always lower case (hex32)
                String id = l.substring(0, tab).toLowerCase(Locale.ROOT);
                if (!ACCOUNT.matcher(id).matches() || id.matches("0+") || ids.contains(id)) continue;
                if (star && marked < 0) marked = ids.size();
                ids.add(id); names.add(l.substring(tab + 1));
            }
        if (!ids.isEmpty()) { int i = Math.max(marked, 0); return new Account(ids.get(i), names.get(i), ids.size(), false); }
        if (Files.exists(uid.toPath(), LinkOption.NOFOLLOW_LINKS)) {
            if (!Files.isRegularFile(uid.toPath(), LinkOption.NOFOLLOW_LINKS)) throw new Problem(BAD_ACCOUNT);
            byte[] raw = Files.readAllBytes(uid.toPath());
            // the game fills an empty one in
            if (raw.length == 0) throw new Problem(START_ONCE);
            if (raw.length != 16) throw new Problem(BAD_ACCOUNT);
            ByteBuffer b = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);
            long lo = b.getLong(), hi = b.getLong();
            if (lo == 0 && hi == 0) throw new Problem(BAD_ACCOUNT);
            return new Account(String.format(Locale.ROOT, "%016x%016x", hi, lo), "Local", 1, false);
        }
        List<String> areas = new ArrayList<>();
        String[] kids = saveDir().list();
        if (kids != null) for (String k : kids) if (ACCOUNT.matcher(k).matches() && !k.matches("0+") && new File(saveDir(), k).isDirectory()) areas.add(k);
        if (areas.size() == 1) return new Account(areas.get(0), "Local", 1, false);
        // the game picks one by directory order, and writes users.txt then
        if (areas.size() > 1) throw new Problem(START_ONCE);
        byte[] id = new byte[16];
        do new SecureRandom().nextBytes(id); while (Arrays.equals(id, new byte[16]));
        return new Account(hex(id), "Local", 1, true);
    }

    // ---- what a save is ----

    /** A stored name as a path inside the save, / between names ("" for "." or "./"). Backslashes count as separators, as
        Windows tools write them. A name that points outside is refused. */
    static String safePath(String raw) throws Problem {
        String p = raw.replace('\\', '/');
        if (p.startsWith("/") || p.matches("^[A-Za-z]:.*")) throw new Problem("This is not safe to open: it has a file with an absolute path (" + raw + ").");
        StringBuilder out = new StringBuilder();
        for (String part : p.split("/")) {
            if (part.equals("..")) throw new Problem("This is not safe to open: it has a file that points outside its folder (" + raw + ").");
            if (part.indexOf('\0') >= 0) throw new Problem(DAMAGED);
            if (part.isEmpty() || part.equals(".")) continue;
            if (out.length() > 0) out.append('/');
            out.append(part);
        }
        return out.toString();
    }

    /** A listing made whole and bounded: folders that are only implied added, names in order, sizes and counts checked. */
    static List<Item> tidy(List<Item> raw) throws Problem {
        TreeMap<String, Item> all = new TreeMap<>(); long bytes = 0;
        for (Item i : raw) {
            if (i.path.isEmpty()) continue;
            Item had = all.put(i.path, i);
            if (had != null && !(had.dir && i.dir)) throw new Problem("This lists a file twice (" + i.path + ").");
            if (i.size > 0) bytes += i.size;
            if (bytes > MAX_BYTES) throw new Problem(TOO_BIG);
        }
        for (Item i : new ArrayList<>(all.values())) {
            int depth = 0;
            for (int at = i.path.indexOf('/'); at >= 0; at = i.path.indexOf('/', at + 1)) {
                if (++depth > MAX_DEPTH) throw new Problem(TOO_MANY);
                String parent = i.path.substring(0, at); Item p = all.get(parent);
                if (p == null) all.put(parent, new Item(parent, true, 0));
                else if (!p.dir) throw new Problem("This has a file and a folder with the same name (" + parent + ").");
            }
        }
        if (all.size() > MAX_FILES) throw new Problem(TOO_MANY);
        return new ArrayList<>(all.values());
    }

    /** Left by file managers, never part of a save. */
    private static boolean junk(String path) {
        for (String part : path.split("/")) if (part.equals("__MACOSX") || part.equals(".DS_Store")) return true;
        return false;
    }

    private static Map<String, Item> byPath(List<Item> items) {
        Map<String, Item> m = new HashMap<>();
        for (Item i : items) m.put(i.path, i);
        return m;
    }

    /** The folder the save sits in: "" when its slots are at the top, or the one folder at the top that has them. */
    static String findRoot(List<Item> items) throws Problem {
        Set<String> parents = new HashSet<>();
        for (Item i : items) {
            if (!i.dir) continue;
            int slash = i.path.indexOf('/');
            if (slash < 0 && SLOT.matcher(i.path).matches()) return "";
            if (slash > 0 && i.path.indexOf('/', slash + 1) < 0 && SLOT.matcher(i.path.substring(slash + 1)).matches()) parents.add(i.path.substring(0, slash));
        }
        if (parents.size() > 1) throw new Problem(TWO_SAVES);
        if (parents.isEmpty()) throw new Problem(NOT_A_SAVE);
        return parents.iterator().next();
    }

    /** The slots with both of their files; populated: every slot with anything in it. */
    private static List<String> slots(List<Item> items, List<String> populated) {
        Map<String, Item> all = byPath(items); List<String> good = new ArrayList<>();
        for (int n = 0; n <= 5; n++) {
            String slot = "slot_0" + n; Item d = all.get(slot);
            if (d == null || !d.dir) continue;
            boolean used = false;
            for (Item i : items) if (i.path.startsWith(slot + "/")) { used = true; break; }
            // empty unused slots are harmless
            if (!used) continue;
            if (populated != null) populated.add(slot);
            Item progress = all.get(slot + "/progress.sav"), caption = all.get(slot + "/caption.sav");
            if (progress != null && !progress.dir && progress.size != 0 && caption != null && !caption.dir && caption.size != 0) good.add(slot);
        }
        return good;
    }

    static List<String> slots(List<Item> items) { return slots(items, null); }

    /** validate_save: the slots, or why this is not a save. items are relative to the save's top. */
    static List<String> validate(List<Item> items) throws Problem {
        List<String> populated = new ArrayList<>(), good = slots(items, populated);
        for (String s : populated) if (!good.contains(s)) throw new Problem("This save is incomplete: " + s + " has no progress.sav or caption.sav, or one of them is empty.");
        if (good.isEmpty()) throw new Problem(NOT_A_SAVE);
        return good;
    }

    // ---- trees on this device ----

    /** Every file and folder under root, in name order. Links are never followed: a link or a special file is refused. */
    static List<Item> tree(File root) throws IOException {
        if (Files.isSymbolicLink(root.toPath())) throw new Problem(LINK);
        List<Item> out = new ArrayList<>();
        walk(root, "", 0, out);
        return out;
    }

    private static void walk(File dir, String prefix, int depth, List<Item> out) throws IOException {
        if (depth > MAX_DEPTH) throw new Problem(TOO_MANY);
        String[] names = dir.list();
        if (names == null) throw new IOException(dir.getPath());
        Arrays.sort(names);
        for (String n : names) {
            File f = new File(dir, n);
            BasicFileAttributes a = Files.readAttributes(f.toPath(), BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
            if (a.isDirectory()) { out.add(new Item(prefix + n, true, 0)); walk(f, prefix + n + "/", depth + 1, out); }
            else if (a.isRegularFile()) out.add(new Item(prefix + n, false, a.size()));
            else throw new Problem(LINK);
            if (out.size() > MAX_FILES) throw new Problem(TOO_MANY);
        }
    }

    /** SHA-256 of every file by path, and each folder as "path/" (empty slots are part of a save), as tree_manifest does. */
    static Map<String, String> sums(File root) throws IOException {
        Map<String, String> m = new TreeMap<>();
        for (Item i : tree(root)) {
            if (i.dir) m.put(i.path + "/", "");
            else try (InputStream in = new FileInputStream(new File(root, i.path))) { m.put(i.path, sha256(in)); }
        }
        return m;
    }

    static String sha256(InputStream in) throws IOException { return pump(in, null, Long.MAX_VALUE); }

    /** Copies in to out (out may be null); returns the bytes' SHA-256. More than limit bytes is a Problem. */
    static String pump(InputStream in, OutputStream out, long limit) throws IOException {
        MessageDigest sha;
        try { sha = MessageDigest.getInstance("SHA-256"); } catch (NoSuchAlgorithmException e) { throw new IOException(e); }
        byte[] buf = new byte[1 << 16]; long total = 0;
        for (int n; (n = in.read(buf)) > 0; ) {
            if ((total += n) > limit) throw new Problem(TOO_BIG);
            sha.update(buf, 0, n);
            if (out != null) out.write(buf, 0, n);
        }
        return hex(sha.digest());
    }

    private static String hex(byte[] bytes) {
        StringBuilder s = new StringBuilder();
        for (byte b : bytes) s.append(String.format(Locale.ROOT, "%02x", b & 0xff));
        return s.toString();
    }

    /** A file on the disk, not only in the cache; returns its SHA-256. */
    private static String write(InputStream in, File to, long limit) throws IOException {
        try (FileOutputStream out = new FileOutputStream(to)) {
            String sum = pump(in, out, limit);
            out.getFD().sync();
            return sum;
        }
    }

    /** The folder's names on the disk. Not every file system takes this, and nothing depends on it but a power cut. */
    private static void sync(File dir) {
        try (FileChannel c = FileChannel.open(dir.toPath(), StandardOpenOption.READ)) { c.force(true); } catch (IOException e) { }
    }

    /** Copies a tree; returns the sums of the bytes read. */
    private static Map<String, String> copyTree(File from, File to) throws IOException {
        Map<String, String> m = new TreeMap<>();
        if (!to.mkdirs()) throw new IOException(to.getPath());
        List<File> dirs = new ArrayList<>(); dirs.add(to);
        for (Item i : tree(from)) {
            File f = new File(to, i.path);
            if (i.dir) { if (!f.mkdir()) throw new IOException(f.getPath()); dirs.add(f); m.put(i.path + "/", ""); }
            else try (InputStream in = new FileInputStream(new File(from, i.path))) { m.put(i.path, write(in, f, Long.MAX_VALUE)); }
        }
        for (File d : dirs) sync(d);
        return m;
    }

    /** Deletes a tree without going through links. */
    static void delete(File f) {
        if (!Files.isSymbolicLink(f.toPath())) { File[] kids = f.listFiles(); if (kids != null) for (File k : kids) delete(k); }
        f.delete();
    }

    private void needSpace(long bytes) throws Problem {
        if (free() < bytes + (32L << 20)) throw new Problem(NO_SPACE);
    }

    private void refuseIfRunning() throws Problem { if (guard.gameRunning()) throw new Problem(RUNNING); }

    // ---- reading a save in ----

    /** A zip, read through its central directory: a zip cut short has none, where reading it entry by entry would end
        cleanly at the cut. Entries are checked against their CRC and size as they are read. */
    static final class ZipSource implements Source, Closeable {
        private final ZipFile zip;
        private final Map<String, ZipEntry> entries = new HashMap<>();
        private final List<Item> items = new ArrayList<>();

        ZipSource(File file) throws IOException {
            if (file.length() > MAX_BYTES) throw new Problem(TOO_BIG);
            checkDirectory(file);
            try { zip = new ZipFile(file); } catch (IOException | RuntimeException e) { throw new Problem(DAMAGED); }
            try {
                for (Enumeration<? extends ZipEntry> all = zip.entries(); all.hasMoreElements(); ) {
                    ZipEntry e = all.nextElement();
                    String path = safePath(e.getName());
                    boolean dir = e.isDirectory() || e.getName().endsWith("\\");
                    if (e.getSize() > MAX_BYTES) throw new Problem(TOO_BIG);
                    items.add(new Item(path, dir, e.getSize()));
                    entries.put(path, e);
                }
            } catch (IOException | RuntimeException e) {
                zip.close();
                if (e instanceof Problem) throw (Problem) e;
                throw new Problem(DAMAGED);
            }
        }

        @Override public List<Item> list() { return items; }

        @Override public InputStream open(String path) throws IOException {
            ZipEntry e = entries.get(path);
            if (e == null) throw new Problem(DAMAGED);
            return new Checked(zip.getInputStream(e), e.getSize(), e.getCrc());
        }

        @Override public void close() throws IOException { zip.close(); }

        /** The central directory, read here because java.util.zip does not show an entry's file type: a link would come out
            as a file holding its target. Names are checked before Android's ZipFile refuses them in words of its own. */
        private static void checkDirectory(File file) throws IOException {
            try (RandomAccessFile f = new RandomAccessFile(file, "r")) {
                long length = f.length(); int tail = (int) Math.min(length, 22 + 65535);
                byte[] end = new byte[tail];
                f.seek(length - tail); f.readFully(end);
                ByteBuffer b = ByteBuffer.wrap(end).order(ByteOrder.LITTLE_ENDIAN);
                int at = -1;
                for (int i = tail - 22; i >= 0 && at < 0; i--) if (b.getInt(i) == 0x06054b50 && i + 22 + (b.getShort(i + 20) & 0xffff) == tail) at = i;
                if (at < 0) throw new Problem(DAMAGED);
                int count = b.getShort(at + 10) & 0xffff;
                long size = b.getInt(at + 12) & 0xffffffffL, offset = b.getInt(at + 16) & 0xffffffffL;
                if (count > MAX_FILES) throw new Problem(TOO_MANY);
                if (offset + size > length - tail + at || size > MAX_FILES * 1024L) throw new Problem(DAMAGED);
                byte[] all = new byte[(int) size];
                f.seek(offset); f.readFully(all);
                ByteBuffer d = ByteBuffer.wrap(all).order(ByteOrder.LITTLE_ENDIAN);
                Set<String> seen = new HashSet<>(); int p = 0;
                for (int n = 0; n < count; n++) {
                    if (p + 46 > all.length || d.getInt(p) != 0x02014b50) throw new Problem(DAMAGED);
                    int host = all[p + 5] & 0xff, name = d.getShort(p + 28) & 0xffff, extra = d.getShort(p + 30) & 0xffff, comment = d.getShort(p + 32) & 0xffff;
                    int type = (d.getInt(p + 38) >>> 16) & 0170000;
                    if (p + 46 + name > all.length) throw new Problem(DAMAGED);
                    String path = safePath(new String(all, p + 46, name, StandardCharsets.UTF_8));
                    if ((host == 3 || host == 19) && type != 0 && type != 0100000 && type != 0040000) throw new Problem(LINK);
                    if (!path.isEmpty() && !seen.add(path)) throw new Problem("This lists a file twice (" + path + ").");
                    p += 46 + name + extra + comment;
                }
            }
        }
    }

    /** ZipFile's streams check nothing: this does, at the end of the entry. */
    private static final class Checked extends FilterInputStream {
        private final CRC32 crc = new CRC32();
        private final long size, want;
        private long read;

        Checked(InputStream in, long size, long want) { super(in); this.size = size; this.want = want; }

        @Override public int read(byte[] b, int off, int len) throws IOException {
            int n;
            try { n = in.read(b, off, len); } catch (ZipException | java.io.EOFException e) { throw new Problem(DAMAGED); }
            if (n > 0) { crc.update(b, off, n); read += n; }
            if (read > size || (n < 0 && (read != size || crc.getValue() != want))) throw new Problem(DAMAGED);
            return n;
        }

        @Override public int read() throws IOException {
            byte[] one = new byte[1];
            return read(one, 0, 1) < 0 ? -1 : one[0] & 0xff;
        }
    }

    /** A folder on this device: a backup, or a tree of test files. */
    static final class FolderSource implements Source {
        private final File root;
        FolderSource(File root) { this.root = root; }
        @Override public List<Item> list() throws IOException { return tree(root); }
        @Override public InputStream open(String path) throws IOException { return new FileInputStream(new File(root, path)); }
    }

    /** Where the picked zip is copied before it is opened: ZipFile needs a file. */
    File picked() throws IOException {
        File dir = staging();
        if (!dir.isDirectory() && !dir.mkdirs()) throw new Problem(NO_SPACE);
        return new File(dir, "picked.zip");
    }

    /** The picked zip, from the system's picker, into picked(). */
    void receive(InputStream in) throws IOException {
        File zip = picked();
        try (FileOutputStream out = new FileOutputStream(zip)) { pump(in, out, MAX_BYTES); }
        catch (IOException e) { zip.delete(); throw e; }
    }

    /** Opens picked() and stages the save in it. */
    Staged stagePicked(Progress p) throws IOException {
        File zip = picked();
        try (ZipSource z = new ZipSource(zip)) { return stage(z, true, p); } finally { zip.delete(); }
    }

    /** Copies a save to the staging folder and checks it; the save in use is not touched. isSave: an import, so the save may
        sit in one parent folder and must pass validate. A backup is taken as it is: it holds whatever the player had. */
    Staged stage(Source source, boolean isSave, Progress p) throws IOException {
        File root = new File(staging(), "save");
        delete(root);
        p.at("Checking the save…");
        List<Item> items = tidy(source.list());
        String from = "";
        if (isSave) {
            List<Item> clean = new ArrayList<>();
            for (Item i : items) if (!junk(i.path)) clean.add(i);
            from = findRoot(clean);
            items = new ArrayList<>();
            for (Item i : clean) {
                if (from.isEmpty()) items.add(i);
                else if (i.path.startsWith(from + "/")) items.add(new Item(i.path.substring(from.length() + 1), i.dir, i.size));
            }
            validate(items);
        }
        long bytes = 0;
        for (Item i : items) if (i.size > 0) bytes += i.size;
        // here, the backup of about the same size, and <account>.new
        needSpace(bytes * 3);
        if (!root.mkdirs()) throw new Problem(NO_SPACE);
        Map<String, String> want = new TreeMap<>();
        long left = MAX_BYTES; int done = 0;
        try {
            for (Item i : items) {
                File to = new File(root, i.path);
                if (!to.toPath().normalize().startsWith(root.toPath().normalize())) throw new Problem(DAMAGED);
                if (i.dir) { if (!to.mkdir()) throw new IOException(to.getPath()); want.put(i.path + "/", ""); continue; }
                try (InputStream in = source.open(from.isEmpty() ? i.path : from + "/" + i.path)) {
                    if (in == null) throw new IOException(i.path);
                    want.put(i.path, write(in, to, left));
                }
                left -= to.length();
                if (++done % 16 == 0) p.at("Copying the save: " + done + " files");
            }
            List<Item> staged = tree(root);
            // again, with the real sizes: a folder's listing may not have them
            if (isSave) validate(staged);
            if (!sums(root).equals(want)) throw new Problem(NOT_COPIED);
            checkpoint("staged");
            return new Staged(root, want, new Info(staged));
        } catch (Problem e) {
            delete(root); throw e;
        } catch (IOException | RuntimeException e) {
            delete(root); throw new Problem(NOT_READ);
        }
    }

    /** The staged save is not wanted after all. */
    void discard() { delete(staging()); }

    // ---- replacing the save ----

    /** Backs the current save up, then swaps the staged one in. keep: a backup rotation must leave alone. */
    void install(Staged s, String keep, Progress p) throws IOException {
        try {
            refuseIfRunning();
            settle();
            Account a = account();
            File live = new File(saveDir(), a.folder), fresh = new File(saveDir(), a.folder + ".new"), old = new File(saveDir(), a.folder + ".old");
            if (!sums(s.root).equals(s.sums)) throw new Problem(NOT_COPIED);
            p.at("Backing up your current save…");
            backup(live, keep);
            checkpoint("backed-up");
            p.at("Copying the save…");
            if (!saveDir().isDirectory() && !saveDir().mkdirs()) throw new Problem(NO_SPACE);
            delete(fresh);
            try {
                if (!copyTree(s.root, fresh).equals(s.sums) || !sums(fresh).equals(s.sums)) throw new Problem(NOT_COPIED);
                checkpoint("verified");
                refuseIfRunning();
                boolean had = Files.exists(live.toPath(), LinkOption.NOFOLLOW_LINKS);
                if (had && !live.renameTo(old)) throw new Problem(NOT_COPIED);
                checkpoint("moved");
                if (!fresh.renameTo(live)) { if (had) old.renameTo(live); throw new Problem(NOT_COPIED); }
            } catch (Problem e) {
                delete(fresh); throw e;
            } catch (IOException | RuntimeException e) {
                delete(fresh); throw new Problem(NOT_COPIED);
            }
            sync(saveDir());
            checkpoint("swapped");
            delete(old);
        } finally { discard(); }
    }

    /** files/save-backups/<time>-profile, checked file by file, then rotated. An account folder with no files in it (the game
        makes one at its first start) has nothing to keep. Returns the backup's name, or null. */
    private String backup(File live, String keep) throws IOException {
        if (!Files.exists(live.toPath(), LinkOption.NOFOLLOW_LINKS)) return null;
        List<Item> items = tree(live); long bytes = 0; boolean any = false;
        for (Item i : items) if (!i.dir) { any = true; bytes += i.size; }
        if (!any) return null;
        needSpace(bytes * 2);
        File dir = backupDir();
        if (!dir.isDirectory() && !dir.mkdirs()) throw new Problem(NO_SPACE);
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date(now())) + "-profile", name = stamp;
        for (int n = 2; new File(dir, name).exists(); n++) name = stamp + "-" + n;
        // saves.py does not list dot names
        File tmp = new File(dir, ".new-" + name), done = new File(dir, name);
        delete(tmp);
        try {
            Map<String, String> read = copyTree(live, tmp);
            if (!sums(tmp).equals(read) || !sums(live).equals(read) || !tmp.renameTo(done)) throw new Problem("Your save could not be backed up, so it was not changed.");
        } catch (Problem e) {
            delete(tmp); throw e;
        } catch (IOException | RuntimeException e) {
            delete(tmp); throw new Problem("Your save could not be backed up, so it was not changed. The device may be out of space.");
        }
        sync(dir);
        rotate(name, keep);
        return name;
    }

    private List<String> backupNames() {
        List<String> names = new ArrayList<>();
        String[] all = backupDir().list();
        if (all != null) for (String n : all) if (!n.startsWith(".") && new File(backupDir(), n).isDirectory()) names.add(n);
        // named by time: the newest first
        Collections.sort(names, Collections.reverseOrder());
        return names;
    }

    /** The newest KEEP stay. The one just made stays whatever the clock says, and so does the one being restored. */
    private void rotate(String made, String keep) {
        int room = KEEP - 1 - (keep != null ? 1 : 0);
        for (String n : backupNames()) {
            if (n.equals(made) || n.equals(keep)) continue;
            if (room > 0) room--; else delete(new File(backupDir(), n));
        }
    }

    /** The backups, newest first. */
    List<Backup> backups() {
        List<Backup> out = new ArrayList<>();
        for (String n : backupNames())
            /* a backup with a link in it: not offered */
            try { out.add(new Backup(n, new Info(tree(new File(backupDir(), n))))); } catch (IOException e) { }
        return out;
    }

    private File backupNamed(String name) throws Problem {
        File f = new File(backupDir(), name);
        if (name.startsWith(".") || !name.equals(f.getName()) || !f.isDirectory()) throw new Problem(NO_BACKUP);
        return f;
    }

    /** Puts a backup back; the save it replaces is backed up first. */
    void restore(String name, Progress p) throws IOException {
        refuseIfRunning();
        File from = backupNamed(name);
        install(stage(new FolderSource(from), false, p), name, p);
    }

    void deleteBackup(String name) throws IOException { delete(backupNamed(name)); }

    // ---- writing a save out ----

    /** The save in use, or null when there is none. */
    Info current() {
        try {
            Account a = account(); File live = new File(saveDir(), a.folder);
            if (a.fresh || !live.isDirectory()) return null;
            Info i = new Info(tree(live));
            return i.files > 0 ? i : null;
        } catch (IOException e) { return null; }
    }

    static String exportName(String title, Date when) {
        return title.replace(' ', '-') + "-save-" + new SimpleDateFormat("yyyy-MM-dd", Locale.ROOT).format(when) + ".zip";
    }

    /** Where the zip is made before it goes to the place the player chose. */
    File exportFile() throws IOException {
        File dir = staging();
        if (!dir.isDirectory() && !dir.mkdirs()) throw new Problem(NO_SPACE);
        return new File(dir, "export.zip");
    }

    /** The save's contents at the zip's top level, read back and checked against the save. */
    void exportZip(File zip, Progress p) throws IOException {
        refuseIfRunning();
        settle();
        Account a = account();
        File live = new File(saveDir(), a.folder);
        if (a.fresh || !live.isDirectory()) throw new Problem(NO_SAVE);
        List<Item> items = tree(live);
        boolean any = false; long bytes = 0;
        for (Item i : items) if (!i.dir) { any = true; bytes += i.size; }
        if (!any || slots(items).isEmpty()) throw new Problem(NO_SAVE);
        validate(items);
        needSpace(bytes);
        p.at("Packing your save…");
        Map<String, String> want = sums(live);
        try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(zip))) {
            for (Item i : items) {
                File f = new File(live, i.path);
                ZipEntry e = new ZipEntry(i.dir ? i.path + "/" : i.path);
                e.setTime(f.lastModified());
                out.putNextEntry(e);
                if (!i.dir) try (InputStream in = new FileInputStream(f)) { pump(in, out, Long.MAX_VALUE); }
                out.closeEntry();
            }
        }
        p.at("Checking the zip…");
        Map<String, String> got = new TreeMap<>();
        try (ZipSource z = new ZipSource(zip)) {
            for (Item i : z.list()) {
                if (i.dir) got.put(i.path + "/", "");
                else try (InputStream in = z.open(i.path)) { got.put(i.path, sha256(in)); }
            }
        }
        if (!got.equals(want)) { zip.delete(); throw new Problem("The zip did not match your save. Try again."); }
    }

    // ---- after an interruption ----

    /** Ends a swap that was cut: the old save goes back if the new one never arrived. */
    private void settle() {
        String[] names = saveDir().list();
        if (names == null) return;
        for (String wanted : new String[]{"old", "new"})
            for (String n : names) {
                Matcher m = SWAP.matcher(n);
                if (!m.matches() || !m.group(2).equals(wanted)) continue;
                File f = new File(saveDir(), n), live = new File(saveDir(), m.group(1));
                if (wanted.equals("old") && !Files.exists(live.toPath(), LinkOption.NOFOLLOW_LINKS)) f.renameTo(live);
                else delete(f);
            }
    }

    /** Before the game can start, and with no task running: settles a swap, and clears what a stopped task left. */
    void recover() {
        settle();
        discard();
        String[] names = backupDir().list();
        if (names != null) for (String n : names) if (n.startsWith(".new-")) delete(new File(backupDir(), n));
    }
}

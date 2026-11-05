package dev.tok.totk;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/** Saves.java on a desktop JVM, on small fake saves made here (no game data): tools/saves_test/run.sh. */
public class SavesTest {
    static final Saves.Progress QUIET = what -> { };
    static final String ACCT = "0123456789abcdef0011223344556677", CANCELLED = "stopped by the test";
    static File base;
    static int passed;
    static final List<String> failed = new ArrayList<>();

    interface Test { void run() throws Exception; }

    static void test(String name, Test t) {
        try { t.run(); passed++; System.out.println("ok    " + name); }
        catch (Exception | AssertionError e) { failed.add(name); System.out.println("FAIL  " + name + ": " + e); e.printStackTrace(System.out); }
    }

    static void check(boolean ok, String what) { if (!ok) throw new AssertionError(what); }
    static void same(Object want, Object got, String what) { if (!want.equals(got)) throw new AssertionError(what + ": wanted " + want + ", got " + got); }

    interface Op { void run() throws IOException; }

    /** The operation must refuse with this message (a part of it). */
    static void refuses(String message, Op op) throws IOException {
        try { op.run(); } catch (Saves.Problem e) { check(e.getMessage().contains(message), "message was: " + e.getMessage()); return; }
        throw new AssertionError("not refused: " + message);
    }

    // ---- fake saves ----

    static byte[] bytes(int seed, int size) { byte[] b = new byte[size]; new Random(seed).nextBytes(b); return b; }

    static void put(File root, String path, byte[] data) throws IOException {
        File f = new File(root, path); f.getParentFile().mkdirs(); Files.write(f.toPath(), data);
    }

    /** Files by path, for a zip or a folder to be built from; a path ending in / is an empty folder. */
    static Map<String, byte[]> fake(int seed) {
        Map<String, byte[]> m = new LinkedHashMap<>();
        m.put("option.sav", bytes(seed, 300));
        m.put("slot_00/progress.sav", bytes(seed + 1, 70000));
        m.put("slot_00/caption.sav", bytes(seed + 2, 500));
        m.put("slot_00/caption.jpg", bytes(seed + 3, 900));
        m.put("slot_03/progress.sav", bytes(seed + 4, 2000));
        m.put("slot_03/caption.sav", bytes(seed + 5, 400));
        m.put("slot_01/", new byte[0]);
        m.put("album/deep/picture.jpg", bytes(seed + 6, 1200));
        return m;
    }

    static void writeFolder(File root, Map<String, byte[]> files) throws IOException {
        for (Map.Entry<String, byte[]> e : files.entrySet())
            if (e.getKey().endsWith("/")) new File(root, e.getKey()).mkdirs(); else put(root, e.getKey(), e.getValue());
    }

    static File zip(String name, String prefix, Map<String, byte[]> files) throws IOException {
        File z = new File(base, name);
        try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(z))) {
            for (Map.Entry<String, byte[]> e : files.entrySet()) { out.putNextEntry(new ZipEntry(prefix + e.getKey())); out.write(e.getValue()); out.closeEntry(); }
        }
        return z;
    }

    static int dirs;
    static File fresh() { File f = new File(base, "files-" + ++dirs); f.mkdirs(); return f; }

    /** A device whose game has saved: users.txt and the save folder. */
    static File device(int seed) throws IOException {
        File files = fresh();
        put(files, "profile/users.txt", ("*" + ACCT + "\tLocal\n").getBytes(StandardCharsets.UTF_8));
        writeFolder(new File(files, "profile/save/" + ACCT), fake(seed));
        return files;
    }

    static File live(File files) { return new File(files, "profile/save/" + ACCT); }

    static Saves saves(File files) { return new Saves(files, () -> false); }

    /** The way the launcher imports: the picked bytes, staged, then installed. */
    static Saves.Staged stage(Saves s, File zip) throws IOException {
        try (InputStream in = new FileInputStream(zip)) { s.receive(in); }
        return s.stagePicked(QUIET);
    }

    static void importZip(Saves s, File zip) throws IOException { s.install(stage(s, zip), null, QUIET); }

    static void noLeftovers(File files) {
        String[] names = new File(files, "profile/save").list();
        if (names != null) for (String n : names) check(!n.contains("."), "left in save/: " + n);
        check(!new File(files, "save-staging").exists() || new File(files, "save-staging").list().length == 0, "staging left");
    }

    // ---- the tests ----

    static void roundTrip() throws IOException {
        File files = device(10), out = new File(base, "export.zip");
        Saves s = saves(files);
        Map<String, String> before = Saves.sums(live(files));
        s.exportZip(out, QUIET);
        same(before, Saves.sums(live(files)), "export left the save alone");
        Map<String, byte[]> unpacked = new LinkedHashMap<>();
        // a plain unzip, as an emulator user would
        try (ZipInputStream in = new ZipInputStream(new FileInputStream(out))) {
            for (ZipEntry e; (e = in.getNextEntry()) != null; ) unpacked.put(e.getName(), in.readAllBytes());
        }
        for (Map.Entry<String, byte[]> e : fake(10).entrySet()) check(Arrays.equals(e.getValue(), unpacked.get(e.getKey())), "in the zip, at its top: " + e.getKey());
        check(unpacked.containsKey("slot_01/"), "the empty slot is in the zip");
        File other = device(20);
        Saves t = saves(other);
        importZip(t, out);
        same(before, Saves.sums(live(other)), "imported bytes");
        for (Map.Entry<String, byte[]> e : fake(10).entrySet())
            if (!e.getKey().endsWith("/")) check(Arrays.equals(e.getValue(), Files.readAllBytes(new File(live(other), e.getKey()).toPath())), e.getKey());
        check(new File(live(other), "slot_01").isDirectory(), "the empty slot came back");
        noLeftovers(other);
    }

    static void zipLayouts() throws IOException {
        Map<String, String> want;
        File source = fresh(); writeFolder(source, fake(30)); want = Saves.sums(source);
        File files = device(31); Saves s = saves(files);
        importZip(s, zip("top.zip", "", fake(30)));
        same(want, Saves.sums(live(files)), "at the top");
        files = device(32); s = saves(files);
        importZip(s, zip("parent.zip", "My Save/", fake(30)));
        same(want, Saves.sums(live(files)), "in a parent folder");
        Map<String, byte[]> junk = new LinkedHashMap<>();
        for (Map.Entry<String, byte[]> e : fake(30).entrySet()) junk.put("backup/" + e.getKey(), e.getValue());
        junk.put("__MACOSX/backup/._option.sav", bytes(1, 10));
        junk.put("backup/.DS_Store", bytes(2, 10));
        junk.put("readme.txt", bytes(3, 10));
        files = device(33); s = saves(files);
        importZip(s, zip("mac.zip", "", junk));
        same(want, Saves.sums(live(files)), "with what a Mac adds, and a file beside the folder");
        Map<String, byte[]> slashes = new LinkedHashMap<>();
        for (Map.Entry<String, byte[]> e : fake(30).entrySet()) slashes.put(e.getKey().replace('/', '\\'), e.getValue());
        files = device(34); s = saves(files);
        importZip(s, zip("windows.zip", "", slashes));
        same(want, Saves.sums(live(files)), "with backslashes");
        Map<String, byte[]> implied = fake(30); implied.remove("slot_01/");
        File noEmpty = fresh(); writeFolder(noEmpty, implied);
        files = device(35); s = saves(files);
        importZip(s, zip("implied.zip", "./", implied));
        same(Saves.sums(noEmpty), Saves.sums(live(files)), "folders only implied, with ./");
    }

    static void folderImport() throws IOException {
        File source = fresh(); writeFolder(new File(source, "parent"), fake(40));
        File files = device(41); Saves s = saves(files);
        s.install(s.stage(new Saves.FolderSource(source), true, QUIET), null, QUIET);
        same(Saves.sums(new File(source, "parent")), Saves.sums(live(files)), "a folder holding the save");
        // a provider that gives no sizes: the real ones are checked after copying
        File empty = fresh(); Map<String, byte[]> bad = fake(42); bad.put("slot_03/caption.sav", new byte[0]); writeFolder(empty, bad);
        Saves.Source noSizes = new Saves.Source() {
            final Saves.FolderSource f = new Saves.FolderSource(empty);
            @Override public List<Saves.Item> list() throws IOException { List<Saves.Item> l = new ArrayList<>(); for (Saves.Item i : f.list()) l.add(new Saves.Item(i.path, i.dir, i.dir ? 0 : -1)); return l; }
            @Override public InputStream open(String path) throws IOException { return f.open(path); }
        };
        Map<String, String> before = Saves.sums(live(files));
        refuses("incomplete", () -> s.stage(noSizes, true, QUIET));
        same(before, Saves.sums(live(files)), "unchanged");
    }

    static void invalidSaves() throws IOException {
        File files = device(50); Saves s = saves(files);
        Map<String, String> before = Saves.sums(live(files));
        Map<String, byte[]> m = new LinkedHashMap<>(); m.put("option.sav", bytes(1, 10)); m.put("picture.jpg", bytes(2, 10));
        File noSlots = zip("noslots.zip", "", m);
        refuses(Saves.NOT_A_SAVE, () -> stage(s, noSlots));
        m = fake(51); m.remove("slot_03/caption.sav");
        File noCaption = zip("nocaption.zip", "", m);
        refuses("slot_03", () -> stage(s, noCaption));
        m = fake(51); m.put("slot_00/progress.sav", new byte[0]);
        File emptyProgress = zip("empty.zip", "", m);
        refuses("slot_00", () -> stage(s, emptyProgress));
        m = new LinkedHashMap<>(); m.put("slot_00/", new byte[0]); m.put("slot_01/", new byte[0]);
        File emptySlots = zip("emptyslots.zip", "", m);
        refuses(Saves.NOT_A_SAVE, () -> stage(s, emptySlots));
        m = new LinkedHashMap<>(); m.put("slot_06/progress.sav", bytes(1, 10)); m.put("slot_06/caption.sav", bytes(2, 10));
        File slot6 = zip("slot6.zip", "", m);
        refuses(Saves.NOT_A_SAVE, () -> stage(s, slot6));
        File twoDeep = zip("deep.zip", "a/b/", fake(51));
        refuses(Saves.NOT_A_SAVE, () -> stage(s, twoDeep));
        m = new LinkedHashMap<>();
        for (Map.Entry<String, byte[]> e : fake(51).entrySet()) { m.put("one/" + e.getKey(), e.getValue()); m.put("two/" + e.getKey(), e.getValue()); }
        File two = zip("two.zip", "", m);
        refuses(Saves.TWO_SAVES, () -> stage(s, two));
        File notZip = new File(base, "notzip.zip"); Files.write(notZip.toPath(), bytes(3, 5000));
        refuses(Saves.DAMAGED, () -> stage(s, notZip));
        File empty = new File(base, "emptyfile.zip"); Files.write(empty.toPath(), new byte[0]);
        refuses(Saves.DAMAGED, () -> stage(s, empty));
        same(before, Saves.sums(live(files)), "the save");
        check(s.backups().isEmpty(), "no backup for a refused import");
    }

    /** A zip whose stored names are swapped, byte for byte, for ones java.util.zip would not write or would clean. */
    static File rawNames(String name, String from, String to) throws IOException {
        check(from.length() == to.length(), "same length");
        Map<String, byte[]> m = fake(60); m.put(from, bytes(4, 20));
        File z = zip(name, "", m);
        byte[] all = Files.readAllBytes(z.toPath()), a = from.getBytes(StandardCharsets.UTF_8), b = to.getBytes(StandardCharsets.UTF_8);
        for (int i = 0; i + a.length <= all.length; i++) if (Arrays.equals(Arrays.copyOfRange(all, i, i + a.length), a)) System.arraycopy(b, 0, all, i, b.length);
        Files.write(z.toPath(), all);
        return z;
    }

    static void unsafeZips() throws IOException {
        File files = device(61); Saves s = saves(files);
        Map<String, String> before = Saves.sums(live(files));
        File outside = new File(files, "escaped.txt");
        refuses("outside its folder", () -> stage(s, rawNames("slip.zip", "xx/yy/escaped.txt", "../../escaped.txt")));
        refuses("outside its folder", () -> stage(s, rawNames("slip2.zip", "slot_00/xx/yy/z", "slot_00/../../z")));
        refuses("outside its folder", () -> stage(s, rawNames("slip3.zip", "xx/yy/escaped.txt", "..\\..\\escaped.txt")));
        refuses("absolute path", () -> stage(s, rawNames("abs.zip", "xtmp/evil.txt", "/tmp/evil.txt")));
        refuses("absolute path", () -> stage(s, rawNames("drive.zip", "xx/evil.txt", "C:/evil.txt")));
        refuses("twice", () -> stage(s, rawNames("twice.zip", "option.sax", "option.sav")));
        Map<String, byte[]> clash = fake(60); clash.put("slot_00/caption.sav/x", bytes(4, 20));
        File clashing = zip("clash.zip", "", clash);
        refuses("same name", () -> stage(s, clashing));
        check(!outside.exists(), "nothing left the folder");
        same(before, Saves.sums(live(files)), "the save");
    }

    static void linksInZips() throws IOException {
        File files = device(62); Saves s = saves(files);
        Map<String, byte[]> m = fake(63); m.put("slot_05/link", "/etc/passwd".getBytes(StandardCharsets.UTF_8));
        File z = zip("link.zip", "", m);
        // mark the entry as a Unix link in the central directory, as zip -y does
        byte[] all = Files.readAllBytes(z.toPath());
        ByteBuffer b = ByteBuffer.wrap(all).order(ByteOrder.LITTLE_ENDIAN);
        boolean marked = false;
        for (int p = 0; p + 46 < all.length; p++) {
            if (b.getInt(p) != 0x02014b50) continue;
            int n = b.getShort(p + 28) & 0xffff;
            if (!new String(all, p + 46, n, StandardCharsets.UTF_8).equals("slot_05/link")) continue;
            all[p + 5] = 3; b.putInt(p + 38, 0120777 << 16); marked = true;
        }
        check(marked, "found the entry");
        Files.write(z.toPath(), all);
        refuses(Saves.LINK, () -> stage(s, z));
    }

    static void linksOnDisk() throws IOException {
        File secret = new File(base, "secret.txt"); Files.write(secret.toPath(), bytes(5, 10));
        File outsideDir = new File(base, "outside-dir"); outsideDir.mkdirs(); put(outsideDir, "keep.txt", bytes(6, 10));
        File folder = fresh(); writeFolder(folder, fake(64));
        Files.createSymbolicLink(new File(folder, "slot_00/link").toPath(), secret.toPath());
        File files = device(65); Saves s = saves(files);
        Map<String, String> before = Saves.sums(live(files));
        refuses(Saves.LINK, () -> s.stage(new Saves.FolderSource(folder), true, QUIET));
        same(before, Saves.sums(live(files)), "the save");
        File linked = device(66);
        Files.createSymbolicLink(new File(live(linked), "album/out").toPath(), outsideDir.toPath());
        Saves t = saves(linked);
        File good = zip("good.zip", "", fake(67));
        // the backup finds the link in the current save
        refuses(Saves.LINK, () -> importZip(t, good));
        check(new File(live(linked), "slot_00/progress.sav").exists(), "the save stayed");
        Saves.delete(live(linked));
        check(new File(outsideDir, "keep.txt").exists(), "deleting does not go through a link");
    }

    static void sizes() throws IOException {
        File files = device(70); Saves s = saves(files);
        Map<String, String> before = Saves.sums(live(files));
        File bomb = new File(base, "bomb.zip");
        try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(bomb))) {
            for (Map.Entry<String, byte[]> e : fake(71).entrySet()) { out.putNextEntry(new ZipEntry(e.getKey())); out.write(e.getValue()); out.closeEntry(); }
            out.putNextEntry(new ZipEntry("slot_00/huge.bin"));
            byte[] zero = new byte[1 << 20];
            for (long n = 0; n <= Saves.MAX_BYTES; n += zero.length) out.write(zero);
            out.closeEntry();
        }
        check(bomb.length() < (16L << 20), "the bomb is small");
        refuses(Saves.TOO_BIG, () -> stage(s, bomb));
        File many = new File(base, "many.zip");
        try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(many))) {
            for (Map.Entry<String, byte[]> e : fake(71).entrySet()) { out.putNextEntry(new ZipEntry(e.getKey())); out.write(e.getValue()); out.closeEntry(); }
            for (int i = 0; i <= Saves.MAX_FILES; i++) { out.putNextEntry(new ZipEntry("album/" + i)); out.closeEntry(); }
        }
        refuses(Saves.TOO_MANY, () -> stage(s, many));
        StringBuilder deep = new StringBuilder();
        for (int i = 0; i <= Saves.MAX_DEPTH + 1; i++) deep.append("d/");
        Map<String, byte[]> m = fake(71); m.put(deep + "f", bytes(1, 5));
        File tooDeep = zip("toodeep.zip", "", m);
        refuses(Saves.TOO_MANY, () -> stage(s, tooDeep));
        // a source that says small and sends more than the limit
        Saves.Source liar = new Saves.Source() {
            @Override public List<Saves.Item> list() {
                List<Saves.Item> l = new ArrayList<>();
                l.add(new Saves.Item("slot_00", true, 0)); l.add(new Saves.Item("slot_00/progress.sav", false, 5)); l.add(new Saves.Item("slot_00/caption.sav", false, 5));
                return l;
            }
            @Override public InputStream open(String path) {
                return new InputStream() {
                    long left = Saves.MAX_BYTES + 2;
                    @Override public int read() { return left-- > 0 ? 7 : -1; }
                    @Override public int read(byte[] b, int off, int len) { if (left <= 0) return -1; int n = (int) Math.min(len, left); left -= n; return n; }
                };
            }
        };
        refuses(Saves.TOO_BIG, () -> s.stage(liar, true, QUIET));
        File big = new File(base, "bigfile.zip");
        try (RandomAccessFile r = new RandomAccessFile(big, "rw")) { r.setLength(Saves.MAX_BYTES + 1); }
        refuses(Saves.TOO_BIG, () -> { try (InputStream in = new FileInputStream(big)) { s.receive(in); } });
        same(before, Saves.sums(live(files)), "the save");
        new Saves(files, () -> false).recover();
        noLeftovers(files);
    }

    static void noSpace() throws IOException {
        File files = device(75);
        long[] free = {1L << 40};
        Saves s = new Saves(files, () -> false) { @Override long free() { return free[0]; } };
        Map<String, String> before = Saves.sums(live(files));
        File good = zip("space.zip", "", fake(76));
        Saves.Staged st = stage(s, good);
        free[0] = 1 << 20;
        refuses(Saves.NO_SPACE, () -> s.install(st, null, QUIET));
        refuses(Saves.NO_SPACE, () -> stage(s, good));
        same(before, Saves.sums(live(files)), "the save");
        check(s.backups().isEmpty(), "no backup");
        noLeftovers(files);
    }

    static void truncatedZips() throws IOException {
        File files = device(80); Saves s = saves(files);
        Map<String, String> before = Saves.sums(live(files));
        File whole = zip("whole.zip", "", fake(81));
        byte[] all = Files.readAllBytes(whole.toPath());
        ByteBuffer b = ByteBuffer.wrap(all).order(ByteOrder.LITTLE_ENDIAN);
        int directory = -1;
        for (int p = all.length - 22; p >= 0; p--) if (b.getInt(p) == 0x06054b50) { directory = b.getInt(p + 16); break; }
        check(directory > 0, "found the directory");
        // entry boundaries, where reading a zip entry by entry would just end, and cuts inside data and inside the directory
        List<Integer> cuts = new ArrayList<>(Arrays.asList(directory, directory + 10, all.length - 1, all.length - 22, 10, all.length / 2));
        for (int p = 4; p + 4 < directory; p++) if (b.getInt(p) == 0x04034b50) cuts.add(p);
        for (int cut : cuts) {
            File z = new File(base, "cut.zip");
            Files.write(z.toPath(), Arrays.copyOf(all, cut));
            refuses(Saves.DAMAGED, () -> stage(s, z));
        }
        byte[] bad = all.clone();
        // in the first entry's data
        bad[100] ^= 0x55;
        File z = new File(base, "flipped.zip");
        Files.write(z.toPath(), bad);
        refuses(Saves.DAMAGED, () -> stage(s, z));
        // a directory that states less than the entry holds: the small size cannot be used to slip more past the limits
        byte[] lying = all.clone();
        ByteBuffer l = ByteBuffer.wrap(lying).order(ByteOrder.LITTLE_ENDIAN);
        for (int p = directory; p + 46 < lying.length; p++) if (l.getInt(p) == 0x02014b50) { l.putInt(p + 24, 5); break; }
        File small = new File(base, "lying.zip");
        Files.write(small.toPath(), lying);
        refuses(Saves.DAMAGED, () -> stage(s, small));
        same(before, Saves.sums(live(files)), "the save");
    }

    static void stoppedImports() throws IOException {
        File files = device(90); Saves s = saves(files);
        Map<String, String> before = Saves.sums(live(files));
        File good = zip("good2.zip", "", fake(91));
        Map<String, byte[]> ok = fake(92);
        // a read that fails half way
        Saves.Source failing = new Saves.Source() {
            int opened;
            final File src = fresh();
            { writeFolder(src, ok); }
            @Override public List<Saves.Item> list() throws IOException { return Saves.tree(src); }
            @Override public InputStream open(String path) throws IOException {
                if (++opened == 3) return new InputStream() { @Override public int read() throws IOException { throw new IOException("gone"); } };
                return new FileInputStream(new File(src, path));
            }
        };
        refuses(Saves.NOT_READ, () -> s.stage(failing, true, QUIET));
        same(before, Saves.sums(live(files)), "after a failed read");
        // the staged copy spoiled before the swap
        Saves.Staged st = stage(s, good);
        Files.write(new File(st.root, "option.sav").toPath(), bytes(9, 300));
        refuses(Saves.NOT_COPIED, () -> s.install(st, null, QUIET));
        same(before, Saves.sums(live(files)), "after a spoiled copy");
        check(s.backups().isEmpty(), "no backup was needed yet");
        noLeftovers(files);
    }

    /** The real thing: another JVM, halted with no cleanup at each step of an import, then the launcher's recover(). */
    static void killedImports() throws Exception {
        String java = System.getProperty("java.home") + "/bin/java", cp = System.getProperty("java.class.path");
        for (String step : new String[]{"staged", "backed-up", "verified", "moved", "swapped"}) {
            File files = device(100), good = zip("killed.zip", "", fake(101));
            Map<String, String> before = Saves.sums(live(files));
            File other = fresh(); writeFolder(other, fake(101));
            Process p = new ProcessBuilder(java, "-cp", cp, "dev.tok.totk.SavesTest", "child", files.getPath(), good.getPath(), step).redirectErrorStream(true).start();
            String said = new String(p.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            same(42, p.waitFor(), "the child stopped at " + step + " (" + said + ")");
            if (step.equals("moved")) check(!live(files).exists(), "this is the moment the save folder is away");
            Saves s = saves(files);
            s.recover();
            same(step.equals("swapped") ? Saves.sums(other) : before, Saves.sums(live(files)), "after " + step);
            noLeftovers(files);
            check(!new File(files, "save-backups").exists() || Arrays.stream(new File(files, "save-backups").list()).noneMatch(n -> n.startsWith(".")), "no half backup");
            // and the next import works
            importZip(s, good);
            same(Saves.sums(other), Saves.sums(live(files)), "import after " + step);
        }
    }

    static void child(String[] a) throws IOException {
        Saves s = new Saves(new File(a[1]), () -> false) { @Override void checkpoint(String where) { if (where.equals(a[3])) Runtime.getRuntime().halt(42); } };
        importZip(s, new File(a[2]));
        System.exit(1);
    }

    static void rotation() throws IOException {
        File files = device(110);
        long[] clock = {1800000000000L};
        Saves s = new Saves(files, () -> false) { @Override long now() { return clock[0] += 60000; } };
        File good = zip("rot.zip", "", fake(111));
        List<String> names = new ArrayList<>();
        for (int i = 0; i < 13; i++) {
            importZip(s, good);
            names.add(s.backups().get(0).name);
        }
        List<Saves.Backup> b = s.backups();
        same(Saves.KEEP, b.size(), "backups kept");
        for (int i = 0; i < Saves.KEEP; i++) same(names.get(12 - i), b.get(i).name, "newest first");
        // restoring the oldest: rotation must not take the backup being restored
        String oldest = b.get(b.size() - 1).name;
        s.restore(oldest, QUIET);
        List<String> after = new ArrayList<>();
        for (Saves.Backup x : s.backups()) after.add(x.name);
        same(Saves.KEEP, after.size(), "still ten");
        check(after.contains(oldest), "the restored one stays");
        // a clock set back: the new backup sorts last, and stays
        clock[0] = 1000000000000L;
        importZip(s, good);
        String made = null;
        for (Saves.Backup x : s.backups()) if (!after.contains(x.name)) made = x.name;
        check(made != null, "the backup made with the clock set back is there");
        same(Saves.KEEP, s.backups().size(), "and still ten");
    }

    static void restoreAndDelete() throws IOException {
        File files = device(120); Saves s = saves(files);
        Map<String, String> first = Saves.sums(live(files));
        importZip(s, zip("r1.zip", "", fake(121)));
        Map<String, String> second = Saves.sums(live(files));
        check(!first.equals(second), "the import changed the save");
        List<Saves.Backup> b = s.backups();
        same(1, b.size(), "one backup");
        same(first, Saves.sums(new File(files, "save-backups/" + b.get(0).name)), "the backup's bytes");
        same(java.util.Arrays.asList("slot_00", "slot_03"), b.get(0).info.slots, "slots");
        check(b.get(0).name.matches("\\d{8}-\\d{6}-profile"), "saves.py's name: " + b.get(0).name);
        s.restore(b.get(0).name, QUIET);
        same(first, Saves.sums(live(files)), "restored");
        same(2, s.backups().size(), "the save it replaced was backed up");
        same(first, Saves.sums(new File(files, "save-backups/" + b.get(0).name)), "the backup is unchanged");
        noLeftovers(files);
        // a backup with no complete slot can still come back: it is what the player had
        File options = device(122);
        Saves.delete(new File(live(options), "slot_00")); Saves.delete(new File(live(options), "slot_03")); Saves.delete(new File(live(options), "slot_01"));
        Map<String, String> onlyOptions = Saves.sums(live(options));
        Saves o = saves(options);
        importZip(o, zip("r2.zip", "", fake(123)));
        s = o;
        s.restore(s.backups().get(0).name, QUIET);
        same(onlyOptions, Saves.sums(live(options)), "a save with no slots");
        String name = s.backups().get(0).name;
        s.deleteBackup(name);
        check(!new File(options, "save-backups/" + name).exists(), "deleted");
        Saves t = s;
        refuses(Saves.NO_BACKUP, () -> t.restore(name, QUIET));
        refuses(Saves.NO_BACKUP, () -> t.restore("../save", QUIET));
        refuses(Saves.NO_BACKUP, () -> t.deleteBackup(".."));
        refuses(Saves.NO_BACKUP, () -> t.deleteBackup("."));
    }

    static void firstRun() throws IOException {
        // no profile at all: the game has never run
        File files = fresh(); Saves s = saves(files);
        check(s.current() == null, "no save");
        File onlyOptions = device(129);
        for (String slot : new String[]{"slot_00", "slot_01", "slot_03"}) Saves.delete(new File(live(onlyOptions), slot));
        refuses(Saves.NO_SAVE, () -> saves(onlyOptions).exportZip(new File(base, "none.zip"), QUIET));
        refuses(Saves.NO_SAVE, () -> s.exportZip(new File(base, "none.zip"), QUIET));
        check(!new File(files, "profile").exists(), "looking changes nothing");
        File good = zip("first.zip", "parent/", fake(130));
        importZip(s, good);
        String[] folders = new File(files, "profile/save").list();
        same(1, folders.length, "one save folder");
        check(folders[0].matches("[0-9a-f]{32}") && !folders[0].matches("0+"), "named as the game names it: " + folders[0]);
        File source = fresh(); writeFolder(source, fake(130));
        same(Saves.sums(source), Saves.sums(new File(files, "profile/save/" + folders[0])), "the save");
        check(!new File(files, "profile/users.txt").exists() && !new File(files, "profile/account.uid").exists(), "the game writes its own account");
        same(folders[0], s.account().folder, "the account is that folder now");
        check(s.backups().isEmpty(), "nothing to back up");
        check(s.current() != null, "and the save is seen");
        // the game ran once and saved nothing: an empty account folder is not worth a backup
        File empty = fresh();
        put(empty, "profile/users.txt", ("*" + ACCT + "\tLocal\n").getBytes(StandardCharsets.UTF_8));
        live(empty).mkdirs();
        Saves e = saves(empty);
        check(e.current() == null, "an empty folder is no save");
        refuses(Saves.NO_SAVE, () -> e.exportZip(new File(base, "none.zip"), QUIET));
        importZip(e, good);
        same(Saves.sums(source), Saves.sums(live(empty)), "into the empty folder");
        check(e.backups().isEmpty(), "no backup of nothing");
    }

    static void accounts() throws IOException {
        File files = fresh(); Saves s = saves(files);
        String a = "aa" + ACCT.substring(2), b = "bb" + ACCT.substring(2);
        File users = new File(files, "profile/users.txt");
        put(files, "profile/users.txt", (a + "\tOne\n*" + b + "\tTwo\n").getBytes(StandardCharsets.UTF_8));
        same(b, s.account().folder, "the marked user"); same("Two", s.account().name, "name"); same(2, s.account().users, "users");
        Files.write(users.toPath(), (a + "\tOne\n" + b + "\tTwo\n").getBytes(StandardCharsets.UTF_8));
        same(a, s.account().folder, "none marked: the first");
        Files.write(users.toPath(), ("# junk\nno tab here\n*zz\tBad\n*" + a.toUpperCase(java.util.Locale.ROOT) + "\tOne\r\n").getBytes(StandardCharsets.UTF_8));
        same(a, s.account().folder, "upper case and CRLF");
        Files.write(users.toPath(), (a + "\tOne\n*" + a + "\tAgain\n" + b + "\tTwo\n").getBytes(StandardCharsets.UTF_8));
        same(a, s.account().folder, "a repeated user is dropped, mark and all");
        StringBuilder nine = new StringBuilder();
        for (int i = 0; i < 8; i++) nine.append(String.format("%032x", i + 1)).append("\tU\n");
        nine.append('*').append(b).append("\tNinth\n");
        Files.write(users.toPath(), nine.toString().getBytes(StandardCharsets.UTF_8));
        same(String.format("%032x", 1), s.account().folder, "the game reads eight users");
        Files.write(users.toPath(), "nothing valid\n".getBytes(StandardCharsets.UTF_8));
        put(files, "profile/account.uid", new byte[]{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, (byte) 0xf0});
        same("f00f0e0d0c0b0a090807060504030201", s.account().folder, "account.uid, as saves.py reads it");
        users.delete();
        same("f00f0e0d0c0b0a090807060504030201", s.account().folder, "account.uid alone");
        File uid = new File(files, "profile/account.uid");
        Files.write(uid.toPath(), new byte[15]);
        refuses(Saves.BAD_ACCOUNT, s::account);
        Files.write(uid.toPath(), new byte[16]);
        refuses(Saves.BAD_ACCOUNT, s::account);
        Files.write(uid.toPath(), new byte[0]);
        refuses(Saves.START_ONCE, s::account);
        uid.delete();
        new File(files, "profile/save/" + a).mkdirs();
        new File(files, "profile/save/" + b + ".new").mkdirs();
        new File(files, "profile/save/not-an-account").mkdirs();
        same(a, s.account().folder, "the one save folder");
        new File(files, "profile/save/" + b).mkdirs();
        refuses(Saves.START_ONCE, s::account);
        // users.txt comes first even with other folders there
        Files.write(users.toPath(), ("*" + b + "\tTwo\n").getBytes(StandardCharsets.UTF_8));
        File good = zip("acct.zip", "", fake(140));
        importZip(s, good);
        File source = fresh(); writeFolder(source, fake(140));
        same(Saves.sums(source), Saves.sums(new File(files, "profile/save/" + b)), "the save went to the user in force");
        same(0, new File(files, "profile/save/" + a).list().length, "the other user's folder was not touched");
        // an old profile: account.uid and no users.txt
        File old = fresh(); Saves o = saves(old);
        put(old, "profile/account.uid", new byte[]{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
        importZip(o, good);
        same(Saves.sums(source), Saves.sums(new File(old, "profile/save/100f0e0d0c0b0a090807060504030201")), "into the old profile's folder");
        check(!new File(old, "profile/users.txt").exists(), "the game writes users.txt itself");
    }

    static void gameRunning() throws IOException {
        File files = device(150);
        boolean[] running = {true};
        Saves s = new Saves(files, () -> running[0]);
        File good = zip("run.zip", "", fake(151));
        Map<String, String> before = Saves.sums(live(files));
        running[0] = false; Saves.Staged st = stage(s, good); running[0] = true;
        refuses(Saves.RUNNING, () -> s.install(st, null, QUIET));
        refuses(Saves.RUNNING, () -> s.exportZip(new File(base, "run-out.zip"), QUIET));
        running[0] = false; importZip(s, good); String backup = s.backups().get(0).name; running[0] = true;
        Map<String, String> imported = Saves.sums(live(files));
        check(!imported.equals(before), "imported");
        refuses(Saves.RUNNING, () -> s.restore(backup, QUIET));
        same(imported, Saves.sums(live(files)), "the save");
        // started at the last moment, after the backup and the copy
        running[0] = false;
        int[] asks = {0};
        Saves late = new Saves(files, () -> ++asks[0] > 1);
        Saves.Staged st2 = new Saves(files, () -> false).stage(new Saves.FolderSource(new File(files, "save-backups/" + backup)), false, QUIET);
        refuses(Saves.RUNNING, () -> late.install(st2, null, QUIET));
        same(imported, Saves.sums(live(files)), "the save, at the last check");
        noLeftovers(files);
    }

    static void small() throws IOException {
        same("slot_00/a", Saves.safePath("./slot_00//a"), "cleaned");
        same("", Saves.safePath("./"), "only a dot");
        same("a/b", Saves.safePath("a\\b"), "backslash");
        same("a..b/c", Saves.safePath("a..b/c"), "dots inside a name are fine");
        same("Tears-of-the-Kingdom-save-2026-11-04.zip", Saves.exportName("Tears of the Kingdom", java.util.Date.from(java.time.LocalDateTime.of(2026, 11, 4, 12, 0).atZone(java.time.ZoneId.systemDefault()).toInstant())), "export name");
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        same(Saves.sha256(new ByteArrayInputStream(bytes(7, 100000))), Saves.pump(new ByteArrayInputStream(bytes(7, 100000)), out, 100000), "pump");
        check(Arrays.equals(bytes(7, 100000), out.toByteArray()), "pump's bytes");
        check(Saves.begin() && !Saves.begin() && Saves.busy(), "one task at a time"); Saves.end(); check(!Saves.busy(), "free again");
    }

    public static void main(String[] a) throws Exception {
        if (a.length > 0 && a[0].equals("child")) { child(a); return; }
        base = Files.createTempDirectory(new File(a[0]).toPath(), "saves-test").toFile();
        test("round trip, identical bytes", SavesTest::roundTrip);
        test("zip layouts: top level, parent folder, Mac extras, backslashes, implied folders", SavesTest::zipLayouts);
        test("folder import", SavesTest::folderImport);
        test("invalid saves are refused", SavesTest::invalidSaves);
        test("zip-slip, absolute paths, repeated names", SavesTest::unsafeZips);
        test("a link in a zip", SavesTest::linksInZips);
        test("links on disk are not followed", SavesTest::linksOnDisk);
        test("unreasonable sizes", SavesTest::sizes);
        test("no free space", SavesTest::noSpace);
        test("truncated and corrupted zips", SavesTest::truncatedZips);
        test("failed imports leave the save", SavesTest::stoppedImports);
        test("killed imports leave the save (separate process, halted at each step)", SavesTest::killedImports);
        test("backup rotation", SavesTest::rotation);
        test("restore and delete", SavesTest::restoreAndDelete);
        test("first run: no save, no account", SavesTest::firstRun);
        test("accounts: users.txt, account.uid, save folders", SavesTest::accounts);
        test("the game running", SavesTest::gameRunning);
        test("small things", SavesTest::small);
        Saves.delete(base);
        System.out.println(passed + " passed, " + failed.size() + " failed" + (failed.isEmpty() ? "" : ": " + failed));
        System.exit(failed.isEmpty() ? 0 : 1);
    }
}

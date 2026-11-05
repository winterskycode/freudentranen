package dev.tok.totk;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.Set;

/** Setup.java on a desktop JVM: tools/setup_test/run.sh. Every key here is made up. */
public class SetupTest {
    // a made-up value: must never show up outside prod.keys
    static final String FAKE_KEY = "00112233445566778899aabbccddeeff";
    static final byte[] GOOD_KEYS = ("header_key = " + FAKE_KEY + FAKE_KEY + "\n").getBytes(StandardCharsets.US_ASCII);
    static final Setup.Progress QUIET = what -> { };
    static final Setup.Stop GO = () -> false;
    static final long GB = 1L << 30;
    static File base;
    static int passed, serial;
    static final List<String> failed = new ArrayList<>();

    interface Test { void run() throws Exception; }

    static void test(String name, Test t) {
        try { t.run(); passed++; System.out.println("ok    " + name); }
        catch (Exception | AssertionError e) { failed.add(name); System.out.println("FAIL  " + name + ": " + e); e.printStackTrace(System.out); }
    }

    static void check(boolean ok, String what) { if (!ok) throw new AssertionError(what); }
    static void same(Object want, Object got, String what) { if (want == null ? got != null : !want.equals(got)) throw new AssertionError(what + ": wanted " + want + ", got " + got); }
    static void contains(String text, String part) { check(text != null && text.contains(part), "\"" + text + "\" does not contain \"" + part + "\""); }

    static File folder() throws IOException { File f = new File(base, "t" + serial++); Files.createDirectories(f.toPath()); return f; }

    static Setup.Info is(long title, int version, int type) { return new Setup.Info(title, version, type, false, 1); }
    static final Setup.Info BASE = is(Setup.TITLE_BASE, Setup.VERSION_BASE, Setup.META_APPLICATION), UPDATE = is(Setup.TITLE_UPDATE, Setup.VERSION_UPDATE, Setup.META_PATCH);
    static final Setup.Info OLD_UPDATE = is(Setup.TITLE_UPDATE, 655360, Setup.META_PATCH), ODD_BASE = is(Setup.TITLE_BASE, 65536, Setup.META_APPLICATION), OTHER = is(0x01006A800016E000L, 0, Setup.META_APPLICATION);
    static final Setup.Info JUNK = new Setup.Info(Setup.E_FORMAT, "this is not an NSP or XCI game file");

    /** A reader with made-up answers, by file. */
    static class Fake implements Setup.Reader {
        final Map<String, Setup.Info> files = new HashMap<>();
        final List<String> opened = new ArrayList<>(), dropped = new ArrayList<>();
        final Set<String> refused = new HashSet<>();
        Setup.Info pairResult = new Setup.Info(0L, 0, 0, false, 0);
        int pairs; byte[] lastKeys; String keysAnswer; Setup.Info keysLocked; long inspectMs;
        @Override public String keysError(byte[] text, int size) { lastKeys = text; return keysAnswer; }
        @Override public Setup.Info inspect(Setup.Doc d, File keys) {
            opened.add(d.uri);
            if (inspectMs > 0) try { Thread.sleep(inspectMs); } catch (InterruptedException e) { throw new IllegalStateException(e); }
            Setup.Info i = files.get(d.uri);
            return i != null ? i : JUNK;
        }
        @Override public Setup.Info pair(Setup.Doc b, Setup.Doc u, File keys) { pairs++; return pairResult; }
        @Override public boolean keep(Setup.Doc d) { return !refused.contains(d.grant); }
        @Override public void drop(String grant) { dropped.add(grant); }
    }

    static Setup.Doc doc(String uri) { return new Setup.Doc(uri, uri, "name of " + uri, 5 * GB); }

    static Setup withKeys(File dir, Fake f) { Setup s = new Setup(dir, f); s.takeKeys(new ByteArrayInputStream(GOOD_KEYS)); return s; }

    static Setup ready(Fake f) throws IOException { f.files.put("b", BASE); f.files.put("u", UPDATE); return withKeys(folder(), f); }

    /** A folder of fake entries: id -> what is in it. */
    static class FakeTree implements Setup.Tree {
        final Map<String, List<Setup.Entry>> folders = new HashMap<>();
        final List<String> listed = new ArrayList<>();
        void add(String parent, Setup.Entry e) { folders.computeIfAbsent(parent, k -> new ArrayList<>()).add(e); }
        void file(String parent, String id, String name, long size) { add(parent, new Setup.Entry(id, name, false, size)); }
        void dir(String parent, String id) { add(parent, new Setup.Entry(id, id, true, 0)); }
        @Override public List<Setup.Entry> children(String id) throws IOException {
            listed.add(id);
            List<Setup.Entry> l = folders.get(id);
            if (l == null) throw new IOException("no folder");
            return l;
        }
        @Override public Setup.Doc doc(Setup.Entry e) { return new Setup.Doc(e.id, "tree", e.name, e.size); }
    }

    // ---- the keys ----

    static void keys() {
        test("good keys are copied with mode 600, and the buffer is wiped", () -> {
            Fake f = new Fake(); File d = folder(); Setup s = withKeys(d, f);
            check(s.keysOk(), "keys not ok");
            check(Arrays.equals(GOOD_KEYS, Files.readAllBytes(s.keysFile().toPath())), "copy differs");
            same("rw-------", PosixFilePermissions.toString(Files.getPosixFilePermissions(s.keysFile().toPath())), "mode");
            for (byte b : f.lastKeys) check(b == 0, "the key text stayed in memory");
            check(!Arrays.asList(d.list()).contains("prod.keys.new"), "temporary file left");
        });
        test("a bad file is refused and changes nothing", () -> {
            Fake f = new Fake(); Setup s = withKeys(folder(), f);
            byte[] before = Files.readAllBytes(s.keysFile().toPath());
            f.keysAnswer = "prod.keys has no header_key";
            s.takeKeys(new ByteArrayInputStream("hello".getBytes(StandardCharsets.US_ASCII)));
            same("This file cannot be used. prod.keys has no header_key. Your earlier keys are still in use.", s.keysProblem(), "message");
            check(Arrays.equals(before, Files.readAllBytes(s.keysFile().toPath())), "the old keys were replaced");
            check(!s.keysOk(), "shown as ok");
            for (byte b : f.lastKeys) check(b == 0, "the bad file's text stayed in memory");
        });
        test("a file too large is not read to its end", () -> {
            Fake f = new Fake(); Setup s = new Setup(folder(), f);
            long[] read = {0};
            s.takeKeys(new InputStream() {
                @Override public int read() { read[0]++; return 'x'; }
                @Override public int read(byte[] b, int off, int n) { read[0] += n; return n; }
            });
            same(Setup.KEYS_TOO_BIG, s.keysProblem(), "message");
            check(read[0] <= Setup.MAX_KEYS + 1, "read " + read[0]);
            check(!s.hasKeys(), "kept");
        });
        test("a read that fails is a message", () -> {
            Fake f = new Fake(); Setup s = new Setup(folder(), f);
            s.takeKeys(new InputStream() { @Override public int read() throws IOException { throw new IOException("gone"); } });
            same(Setup.KEYS_UNREADABLE, s.keysProblem(), "message"); check(!s.hasKeys(), "kept");
        });
        test("keys that cannot be written leave a message and no file", () -> {
            Fake f = new Fake(); File d = folder(); Setup s = new Setup(new File(d, "x"), f);
            // a file where the folder should be
            Files.write(new File(d, "x").toPath(), new byte[1]);
            s.takeKeys(new ByteArrayInputStream(GOOD_KEYS));
            same(Setup.KEYS_NOT_SAVED, s.keysProblem(), "message");
        });
        test("key values reach no message and no other file", () -> {
            Fake f = new Fake(); File d = folder(); Setup s = withKeys(d, f);
            f.files.put("b", BASE); f.files.put("u", UPDATE);
            s.takeFiles(Arrays.asList(doc("b"), doc("u")), Setup.BASE);
            s.report(Setup.BASE);
            for (String n : d.list()) if (!n.equals("prod.keys")) check(!new String(Files.readAllBytes(new File(d, n).toPath()), StandardCharsets.ISO_8859_1).contains(FAKE_KEY), n + " holds a key value");
            for (String m : new String[] {s.keysProblem(), s.problem(Setup.BASE), s.problem(Setup.UPDATE)}) check(m == null || !m.contains(FAKE_KEY), "message holds a key value");
        });
    }

    // ---- files picked one by one or together ----

    static void files() {
        test("base and update in one pick, in either order", () -> {
            for (boolean flip : new boolean[] {false, true}) {
                Fake f = new Fake(); Setup s = ready(f);
                s.takeFiles(flip ? Arrays.asList(doc("u"), doc("b")) : Arrays.asList(doc("b"), doc("u")), Setup.BASE);
                check(s.complete(), "not complete"); same("b", s.doc(Setup.BASE).uri, "base"); same("u", s.doc(Setup.UPDATE).uri, "update");
                same(1, f.pairs, "pair checks");
            }
        });
        test("one at a time, whichever row was pressed", () -> {
            Fake f = new Fake(); Setup s = ready(f);
            s.takeFiles(Collections.singletonList(doc("u")), Setup.BASE);
            check(s.ok(Setup.UPDATE) && s.doc(Setup.BASE) == null && !s.complete(), "update not placed");
            s.takeFiles(Collections.singletonList(doc("b")), Setup.UPDATE);
            check(s.complete(), "not complete");
        });
        test("the picks survive a restart and the game's process sees them", () -> {
            Fake f = new Fake(); File d = folder(); Setup s = withKeys(d, f);
            f.files.put("b", BASE); f.files.put("u", UPDATE);
            s.takeFiles(Arrays.asList(doc("b"), doc("u")), Setup.BASE);
            Setup game = new Setup(d, null);
            check(game.complete(), "not complete"); same("b", game.doc(Setup.BASE).uri, "base"); same(5 * GB, game.doc(Setup.UPDATE).size, "size");
            same("name of u", game.doc(Setup.UPDATE).name, "name");
        });
        test("the update is another version", () -> {
            Fake f = new Fake(); Setup s = ready(f); f.files.put("old", OLD_UPDATE);
            s.takeFiles(Collections.singletonList(doc("old")), Setup.UPDATE);
            same(Setup.OTHER_UPDATE, s.problem(Setup.UPDATE), "message"); check(s.doc(Setup.UPDATE) == null, "taken");
        });
        test("a base game of another version", () -> {
            Fake f = new Fake(); Setup s = ready(f); f.files.put("odd", ODD_BASE);
            s.takeFiles(Collections.singletonList(doc("odd")), Setup.BASE);
            same(Setup.OTHER_BASE, s.problem(Setup.BASE), "message");
        });
        test("base and update the same kind", () -> {
            Fake f = new Fake(); Setup s = ready(f); f.files.put("b2", BASE); f.files.put("u2", UPDATE);
            s.takeFiles(Arrays.asList(doc("b"), doc("b2")), Setup.BASE);
            same(Setup.BOTH_BASE, s.problem(Setup.UPDATE), "two bases"); check(s.ok(Setup.BASE), "base");
            f = new Fake(); s = ready(f); f.files.put("u2", UPDATE);
            s.takeFiles(Arrays.asList(doc("u"), doc("u2")), Setup.BASE);
            same(Setup.BOTH_UPDATE, s.problem(Setup.BASE), "two updates"); check(s.ok(Setup.UPDATE), "update");
        });
        test("the wrong game", () -> {
            Fake f = new Fake(); Setup s = ready(f); f.files.put("x", OTHER);
            s.takeFiles(Collections.singletonList(doc("x")), Setup.BASE);
            same(Setup.OTHER_GAME, s.problem(Setup.BASE), "message");
        });
        test("not a game file goes on the row pressed", () -> {
            Fake f = new Fake(); Setup s = ready(f);
            s.takeFiles(Collections.singletonList(doc("junk")), Setup.UPDATE);
            same(Setup.NOT_GAME, s.problem(Setup.UPDATE), "message"); same(null, s.problem(Setup.BASE), "base");
        });
        test("two bad files fill both rows, and a good one's row is left to it", () -> {
            Fake f = new Fake(); Setup s = ready(f); f.files.put("x", OTHER);
            s.takeFiles(Arrays.asList(doc("junk"), doc("x")), Setup.BASE);
            same(Setup.NOT_GAME, s.problem(Setup.BASE), "first"); same(Setup.OTHER_GAME, s.problem(Setup.UPDATE), "second");
            f = new Fake(); s = ready(f);
            s.takeFiles(Arrays.asList(doc("b"), doc("junk")), Setup.BASE);
            check(s.ok(Setup.BASE), "base"); same(Setup.NOT_GAME, s.problem(Setup.UPDATE), "update row");
        });
        test("a bad new pick leaves the old one and says so", () -> {
            Fake f = new Fake(); Setup s = ready(f); f.files.put("old", OLD_UPDATE);
            s.takeFiles(Arrays.asList(doc("b"), doc("u")), Setup.BASE);
            s.takeFiles(Collections.singletonList(doc("old")), Setup.UPDATE);
            contains(s.problem(Setup.UPDATE), Setup.OTHER_UPDATE); contains(s.problem(Setup.UPDATE), "Still using name of u.");
            same("u", s.doc(Setup.UPDATE).uri, "pick"); check(s.complete(), "the good pair went");
        });
        test("game files before keys", () -> {
            Fake f = new Fake(); Setup s = new Setup(folder(), f);
            s.takeFiles(Collections.singletonList(doc("b")), Setup.BASE);
            same(Setup.KEYS_FIRST, s.keysProblem(), "message"); check(f.opened.isEmpty(), "opened");
        });
        test("keys that lack a key: the keys' row says which, and new keys finish the job", () -> {
            Fake f = new Fake(); Setup s = ready(f);
            f.files.put("u", new Setup.Info(Setup.E_KEY_MISSING, "prod.keys has no titlekek_0f"));
            s.takeFiles(Arrays.asList(doc("b"), doc("u")), Setup.BASE);
            contains(s.keysProblem(), "titlekek_0f"); same(Setup.NEEDS_KEYS, s.problem(Setup.UPDATE), "file row"); check(!s.complete(), "complete");
            f.files.put("u", UPDATE);
            s.takeKeys(new ByteArrayInputStream(GOOD_KEYS));
            same(null, s.keysProblem(), "keys"); check(s.complete(), "not complete");
        });
        test("what the pair check finds goes on the right row, and clears", () -> {
            Fake f = new Fake(); Setup s = ready(f);
            f.pairResult = new Setup.Info(Setup.E_KEY_MISSING, "prod.keys has no key_area_key_application_12");
            s.takeFiles(Arrays.asList(doc("b"), doc("u")), Setup.BASE);
            contains(s.keysProblem(), "key_area_key_application_12"); check(!s.complete(), "complete");
            f.pairResult = new Setup.Info(Setup.E_KEY_WRONG, "the patched RomFS does not decrypt: the keys or the base game do not match the update");
            s.takeFiles(Collections.singletonList(doc("u")), Setup.UPDATE);
            same(Setup.KEYS_WRONG, s.keysProblem(), "wrong keys");
            f.pairResult = new Setup.Info(Setup.E_FORMAT, "the update has no patched RomFS");
            s.takeFiles(Collections.singletonList(doc("u")), Setup.UPDATE);
            same(null, s.keysProblem(), "keys row"); same(Setup.DAMAGED, s.problem(Setup.UPDATE), "update row");
            f.pairResult = new Setup.Info(Setup.E_TICKET, "the file has no ticket for its content");
            s.takeFiles(Collections.singletonList(doc("b")), Setup.BASE);
            same("The file has no ticket for its content.", s.problem(Setup.BASE), "ticket"); same(s.problem(Setup.BASE), s.problem(Setup.UPDATE), "both rows");
            f.pairResult = new Setup.Info(0L, 0, 0, false, 0);
            s.takeFiles(Collections.singletonList(doc("b")), Setup.BASE);
            same(null, s.problem(Setup.BASE), "base clears"); same(null, s.problem(Setup.UPDATE), "update clears"); check(s.complete(), "not complete");
        });
        test("permissions: kept, refused, dropped once nothing uses them", () -> {
            Fake f = new Fake(); Setup s = ready(f); f.files.put("b2", BASE); f.refused.add("u");
            s.takeFiles(Arrays.asList(doc("b"), doc("u")), Setup.BASE);
            same(Setup.NOT_KEPT, s.problem(Setup.UPDATE), "refused"); check(s.doc(Setup.UPDATE) == null && !s.complete(), "taken");
            s.takeFiles(Collections.singletonList(doc("b2")), Setup.BASE);
            same(Collections.singletonList("b"), f.dropped, "dropped");
            Fake g = new Fake(); Setup t = ready(g); g.files.put("b2", BASE);
            t.takeFiles(Arrays.asList(new Setup.Doc("b", "tree", "b", 1), new Setup.Doc("u", "tree", "u", 1)), Setup.BASE);
            t.takeFiles(Collections.singletonList(doc("b2")), Setup.BASE);
            check(g.dropped.isEmpty(), "the folder's permission went while the update needs it");
        });
        test("a file gone at the start: reported by the game's process, shown by the launcher, the pick stands", () -> {
            Fake f = new Fake(); File d = folder(); Setup s = withKeys(d, f);
            f.files.put("b", BASE); f.files.put("u", UPDATE);
            s.takeFiles(Arrays.asList(doc("b"), doc("u")), Setup.BASE);
            new Setup(d, null).report(Setup.UPDATE);
            check(s.takeReported(), "not told"); same(Setup.UPDATE_GONE, s.problem(Setup.UPDATE), "message"); check(!s.takeReported(), "told twice");
            check(s.complete(), "the pick went");
        });
        test("a damaged saved state is no picks, never a crash", () -> {
            Random r = new Random(11);
            for (int i = 0; i < 300; i++) {
                File d = folder(); byte[] b = new byte[r.nextInt(200)]; r.nextBytes(b);
                String text = i % 3 == 0 ? "v1\nchecked\nbase\tx\ty\tz\tn\n" : i % 3 == 1 ? "v1\nbase\t1\n\n\n" : new String(b, StandardCharsets.ISO_8859_1);
                Files.write(new File(d, "game-files.txt").toPath(), text.getBytes(StandardCharsets.ISO_8859_1));
                Files.write(new File(d, "game-files-problem.txt").toPath(), b);
                Setup s = new Setup(d, new Fake());
                check(!s.complete(), "complete from junk"); s.takeReported();
            }
        });
        test("a file name with odd characters", () -> {
            Fake f = new Fake(); File d = folder(); Setup s = withKeys(d, f); f.files.put("b", BASE);
            s.takeFiles(Collections.singletonList(new Setup.Doc("b", "b", "a\tb\nc", 1)), Setup.BASE);
            same("a b c", new Setup(d, null).doc(Setup.BASE).name, "name");
        });
    }

    // ---- a picked folder ----

    static void folders() {
        test("both found by content under any names, in subfolders", () -> {
            Fake f = new Fake(); Setup s = ready(f); FakeTree t = new FakeTree();
            f.files.put("x1", BASE); f.files.put("x2", UPDATE);
            t.dir("root", "sub"); t.file("root", "x2", "base.nsp", GB); t.file("sub", "x1", "update.nsp", 16 * GB);
            s.takeFolder(t, "root", QUIET, GO);
            check(s.complete(), "not complete"); same("x1", s.doc(Setup.BASE).uri, "base"); same("x2", s.doc(Setup.UPDATE).uri, "update");
            same("tree", s.doc(Setup.BASE).grant, "grant");
        });
        test("many large unrelated files: largest first, small ones never opened, stops when both are found", () -> {
            Fake f = new Fake(); Setup s = ready(f); FakeTree t = new FakeTree();
            for (int i = 0; i < 3000; i++) t.file("root", "small" + i, "photo", 1 << 20);
            for (int i = 0; i < 50; i++) t.file("root", "film" + i, "film", 2 * GB);
            t.file("root", "the-base", "x", 16 * GB); t.file("root", "the-update", "y", 3 * GB);
            f.files.put("the-base", BASE); f.files.put("the-update", UPDATE);
            s.takeFolder(t, "root", QUIET, GO);
            check(s.complete(), "not complete");
            same(Arrays.asList("the-base", "the-update"), f.opened, "opened");
        });
        test("a folder of small files: none is opened, and a size the provider does not give is looked at", () -> {
            Fake f = new Fake(); Setup s = ready(f); FakeTree t = new FakeTree();
            for (int i = 0; i < 3000; i++) t.file("root", "small" + i, "photo", Setup.MIN_SIZE - 1);
            s.takeFolder(t, "root", QUIET, GO);
            check(f.opened.isEmpty(), "opened " + f.opened.size()); same(Setup.NOT_IN_FOLDER, s.problem(Setup.BASE), "message");
            t.file("root", "no-size", "x", -1); f.files.put("no-size", BASE);
            s.takeFolder(t, "root", QUIET, GO);
            check(s.ok(Setup.BASE), "the file with no size");
        });
        test("a folder of nothing but big other files is given up on after MAX_OPENED", () -> {
            Fake f = new Fake(); Setup s = ready(f); FakeTree t = new FakeTree();
            for (int i = 0; i < 5000; i++) t.file("root", "film" + i, "film", 2 * GB);
            s.takeFolder(t, "root", QUIET, GO);
            same(Setup.MAX_OPENED, f.opened.size(), "opened"); same(Setup.TOO_MANY, s.problem(Setup.BASE), "base"); same(Setup.TOO_MANY, s.problem(Setup.UPDATE), "update");
        });
        test("a walk that takes too long ends with a message", () -> {
            Fake f = new Fake(); Setup s = ready(f); FakeTree t = new FakeTree();
            for (int i = 0; i < 100; i++) t.file("root", "film" + i, "film", 2 * GB);
            s.scanLimitMs = 40; f.inspectMs = 15;
            s.takeFolder(t, "root", QUIET, GO);
            check(f.opened.size() < 20, "opened " + f.opened.size()); same(Setup.TOO_MANY, s.problem(Setup.BASE), "message");
        });
        test("a loop of folders ends, and huge listings end", () -> {
            Fake f = new Fake(); Setup s = ready(f); FakeTree t = new FakeTree();
            t.dir("root", "a"); t.dir("a", "root");
            s.takeFolder(t, "root", QUIET, GO);
            same(Arrays.asList("root", "a"), t.listed, "listed");
            same(Setup.NOT_IN_FOLDER, s.problem(Setup.BASE), "empty folder");
            FakeTree deep = new FakeTree(); for (int i = 0; i < 40; i++) deep.dir(i == 0 ? "root" : "d" + (i - 1), "d" + i); deep.folders.put("d39", new ArrayList<>());
            s.takeFolder(deep, "root", QUIET, GO);
            check(deep.listed.size() <= Setup.MAX_DEPTH + 1, "deep " + deep.listed.size());
            FakeTree wide = new FakeTree(); for (int i = 0; i < Setup.MAX_LISTED + 500; i++) wide.file("root", "f" + i, "f", 1);
            s.takeFolder(wide, "root", QUIET, GO);
            same(Setup.TOO_MANY, s.problem(Setup.UPDATE), "wide");
        });
        test("a subfolder that cannot be read is passed over; the folder itself is a message", () -> {
            Fake f = new Fake(); Setup s = ready(f); FakeTree t = new FakeTree();
            t.dir("root", "locked"); t.file("root", "x", "x", 16 * GB); f.files.put("x", BASE);
            s.takeFolder(t, "root", QUIET, GO);
            check(s.ok(Setup.BASE), "base"); same(Setup.NOT_IN_FOLDER, s.problem(Setup.UPDATE), "update");
            s.takeFolder(new FakeTree(), "root", QUIET, GO);
            contains(s.problem(Setup.UPDATE), Setup.NOT_LISTED); contains(s.problem(Setup.BASE), "Still using");
        });
        test("found nearly: an update of another version, another game, keys that cannot read it", () -> {
            Fake f = new Fake(); Setup s = ready(f); FakeTree t = new FakeTree();
            t.file("root", "a", "a", 16 * GB); t.file("root", "o", "o", GB); f.files.put("a", BASE); f.files.put("o", OLD_UPDATE);
            s.takeFolder(t, "root", QUIET, GO);
            same(Setup.OTHER_UPDATE, s.problem(Setup.UPDATE), "other version");
            f = new Fake(); s = ready(f); t = new FakeTree(); t.file("root", "x", "x", GB); f.files.put("x", OTHER);
            s.takeFolder(t, "root", QUIET, GO);
            same(Setup.OTHER_GAME, s.problem(Setup.BASE), "other game");
            f = new Fake(); s = ready(f); t = new FakeTree(); t.file("root", "x", "x", GB); f.files.put("x", new Setup.Info(Setup.E_KEY_MISSING, "prod.keys has no titlekek_0f"));
            s.takeFolder(t, "root", QUIET, GO);
            contains(s.keysProblem(), "titlekek_0f"); same(Setup.NEEDS_KEYS, s.problem(Setup.BASE), "needs keys");
        });
        test("an unrelated file that keys cannot read blames nothing when both are found", () -> {
            Fake f = new Fake(); Setup s = ready(f); FakeTree t = new FakeTree();
            t.file("root", "x", "x", 20 * GB); t.file("root", "the-base", "a", 16 * GB); t.file("root", "the-update", "b", GB);
            f.files.put("x", new Setup.Info(Setup.E_KEY_MISSING, "prod.keys has no titlekek_1f")); f.files.put("the-base", BASE); f.files.put("the-update", UPDATE);
            s.takeFolder(t, "root", QUIET, GO);
            same(null, s.keysProblem(), "keys"); check(s.complete(), "not complete");
        });
        test("stopped: nothing changes and nothing is said", () -> {
            Fake f = new Fake(); Setup s = ready(f); FakeTree t = new FakeTree();
            for (int i = 0; i < 100; i++) t.file("root", "film" + i, "film", 2 * GB);
            s.takeFolder(t, "root", QUIET, () -> f.opened.size() >= 3);
            same(3, f.opened.size(), "opened"); same(null, s.problem(Setup.BASE), "message"); check(s.doc(Setup.BASE) == null, "pick");
        });
    }

    // ---- what the player reads ----

    static void words() {
        test("every message is short enough for its row and points nowhere", () -> {
            List<String> all = new ArrayList<>();
            for (Field f : Setup.class.getDeclaredFields())
                if (Modifier.isStatic(f.getModifiers()) && f.getType() == String.class) { f.setAccessible(true); all.add((String) f.get(null)); }
            check(all.size() > 15, "fields");
            String[][] reader = {{"-3", "this is not an NSP or XCI game file"}, {"-3", "this game file is compressed; only plain NSP and XCI files can be read in place"},
                {"-3", "an NCA's size does not match its place in the file: the file is damaged"}, {"-3", "PFS0 entry runs past the end of the file"}, {"-3", "no HFS0 table where there should be one"},
                {"-5", "prod.keys has no titlekek_0f"}, {"-6", "no NCA in this file decrypts: it is not a game file, or header_key in prod.keys is wrong"},
                {"-6", "the keys do not decrypt the content meta: prod.keys does not match this file"}, {"-7", "the file's ticket is tied to one console; a common ticket is needed"},
                {"-2", "cannot read the game file"}, {"-8", "the file given as the base game is not Tears of the Kingdom (title 01006a800016e000)"},
                {"-9", "the update is not 1.4.3 (its version number is 655360, 1.4.3 has 720896)"}, {"-9", "the base game is not 1.0.0 (its version number is 65536)"},
                {"-10", "the file given as the update is the base game"}, {"-10", "the file given as the base game is the update"}, {"-3", ""}, {"-100", ""}, {"-101", ""}, {"-102", ""}};
            for (String[] r : reader) all.add(Setup.plain(new Setup.Info(Integer.parseInt(r[0]), r[1])));
            for (String m : all) {
                check(m.length() <= 170, "too long for two lines: " + m);
                String l = m.toLowerCase(java.util.Locale.ROOT);
                for (String word : new String[] {"http", "dump", "download", "firmware", "pc ", "computer", "hack", "nca", "pfs0", "hfs0", "romfs", "exefs", "title 0"})
                    check(!l.contains(word), "\"" + word + "\" in: " + m);
            }
        });
        test("compressed files and split files say something useful", () -> {
            contains(Setup.plain(new Setup.Info(Setup.E_FORMAT, "this game file is compressed; only plain NSP and XCI files can be read in place")), "compressed");
            same(Setup.DAMAGED, Setup.plain(new Setup.Info(Setup.E_FORMAT, "PFS0 entry runs past the end of the file")), "cut short");
        });
    }

    // ---- the real reader through JNI, made-up game files ----

    static int fd(RandomAccessFile f) throws Exception {
        Field n = FileDescriptor.class.getDeclaredField("fd"); n.setAccessible(true);
        return n.getInt(f.getFD());
    }

    static long bytesRead() throws IOException {
        for (String l : Files.readAllLines(new File("/proc/self/io").toPath())) if (l.startsWith("rchar:")) return Long.parseLong(l.substring(6).trim());
        throw new IOException("no rchar");
    }

    /** The reader in libtotksetup.so, with descriptors of plain files in place of a document provider's. */
    static class Real implements Setup.Reader {
        @Override public String keysError(byte[] text, int size) { return SetupNative.keysError(text, size); }
        @Override public Setup.Info inspect(Setup.Doc d, File keys) {
            try (RandomAccessFile f = new RandomAccessFile(d.uri, "r")) {
                long[] o = new long[6];
                String text = SetupNative.inspect(keys.getPath(), fd(f), o);
                return text != null ? new Setup.Info((int) o[0], text) : new Setup.Info(o[1], (int) o[2], (int) o[3], o[4] != 0, (int) o[5]);
            } catch (Exception e) { return new Setup.Info(Setup.E_NOT_OPENED, null); }
        }
        @Override public Setup.Info pair(Setup.Doc b, Setup.Doc u, File keys) {
            try (RandomAccessFile x = new RandomAccessFile(b.uri, "r"); RandomAccessFile y = new RandomAccessFile(u.uri, "r")) {
                long[] o = new long[1];
                String text = SetupNative.pair(keys.getPath(), fd(x), fd(y), o);
                return text != null ? new Setup.Info((int) o[0], text) : new Setup.Info(0L, 0, 0, false, 0);
            } catch (Exception e) { return new Setup.Info(Setup.E_NOT_OPENED, null); }
        }
        @Override public boolean keep(Setup.Doc d) { return true; }
        @Override public void drop(String grant) { }
    }

    static Setup.Doc real(File f) { return new Setup.Doc(f.getPath(), f.getPath(), f.getName(), f.length()); }

    /** A folder of real files, as a document tree would show it. */
    static class RealTree implements Setup.Tree {
        @Override public List<Setup.Entry> children(String id) throws IOException {
            File[] in = new File(id).listFiles(); if (in == null) throw new IOException();
            List<Setup.Entry> out = new ArrayList<>();
            for (File f : in) out.add(new Setup.Entry(f.getPath(), f.getName(), f.isDirectory(), f.length()));
            return out;
        }
        @Override public Setup.Doc doc(Setup.Entry e) { return new Setup.Doc(e.id, "tree", e.name, e.size); }
    }

    static void reader(File fakes) {
        if (!SetupNative.load()) { failed.add("libtotksetup.so did not load"); return; }
        File keysFile = new File(fakes, "keys.txt");
        test("the reader: keys", () -> {
            byte[] good = Files.readAllBytes(keysFile.toPath());
            same(null, SetupNative.keysError(good, good.length), "made-up keys");
            contains(SetupNative.keysError(new byte[0], 0), "header_key");
            contains(SetupNative.keysError(new byte[5000], 5000), "header_key");
            byte[] bad = "header_key = 12\n".getBytes(StandardCharsets.US_ASCII);
            String why = SetupNative.keysError(bad, bad.length);
            contains(why, "header_key"); check(!why.contains(" 12"), "a value in: " + why);
            byte[] secret = ("master_key_00 = " + FAKE_KEY + "\nheader_key = " + FAKE_KEY + "\n").getBytes(StandardCharsets.US_ASCII);
            why = SetupNative.keysError(secret, secret.length);
            contains(why, "header_key"); check(!why.contains(FAKE_KEY), "a value in the error");
        });
        test("the reader: every made-up file sorted by its content, whatever it is called", () -> {
            Setup s = new Setup(folder(), new Real());
            s.takeKeys(Files.newInputStream(keysFile.toPath()));
            check(s.keysOk(), s.keysProblem());
            File swapped = new File(folder(), "base.nsp"); Files.copy(new File(fakes, "update.nsp").toPath(), swapped.toPath());
            s.takeFiles(Arrays.asList(real(swapped), real(new File(fakes, "base.nsp"))), Setup.BASE);
            check(s.complete(), s.keysProblem() + " " + s.problem(Setup.BASE) + " " + s.problem(Setup.UPDATE));
            same(swapped.getPath(), s.doc(Setup.UPDATE).uri, "named base.nsp, is the update");
            s = new Setup(folder(), new Real()); s.takeKeys(Files.newInputStream(keysFile.toPath()));
            s.takeFiles(Arrays.asList(real(new File(fakes, "base.xci")), real(new File(fakes, "update.nsp"))), Setup.BASE);
            check(s.complete(), "XCI: " + s.problem(Setup.BASE) + " " + s.problem(Setup.UPDATE));
        });
        test("the reader: wrong files give the row's message", () -> {
            Setup s = new Setup(folder(), new Real()); s.takeKeys(Files.newInputStream(keysFile.toPath()));
            s.takeFiles(Collections.singletonList(real(new File(fakes, "older-update.nsp"))), Setup.UPDATE);
            same(Setup.OTHER_UPDATE, s.problem(Setup.UPDATE), "older update");
            s.takeFiles(Collections.singletonList(real(new File(fakes, "newer-base.nsp"))), Setup.BASE);
            same(Setup.OTHER_BASE, s.problem(Setup.BASE), "newer base");
            s.takeFiles(Collections.singletonList(real(new File(fakes, "other-game.nsp"))), Setup.BASE);
            same(Setup.OTHER_GAME, s.problem(Setup.BASE), "other game");
            s.takeFiles(Collections.singletonList(real(keysFile)), Setup.BASE);
            same(Setup.NOT_GAME, s.problem(Setup.BASE), "not a game file");
        });
        test("the reader: keys that do not fit", () -> {
            String text = new String(Files.readAllBytes(keysFile.toPath()), StandardCharsets.US_ASCII);
            Setup s = new Setup(folder(), new Real());
            s.takeKeys(new ByteArrayInputStream(text.replaceAll("(?m)^titlekek.*\n", "").replaceAll("(?mi)^key_area.*\n", "").getBytes(StandardCharsets.US_ASCII)));
            check(s.hasKeys(), "keys refused: " + s.keysProblem());
            s.takeFiles(Arrays.asList(real(new File(fakes, "base.nsp")), real(new File(fakes, "update.nsp"))), Setup.BASE);
            contains(s.keysProblem(), "Your prod.keys lacks "); contains(s.keysProblem(), "which this game needs"); check(!s.complete(), "complete");
            // the right ones: no need to pick the files again
            s.takeKeys(Files.newInputStream(keysFile.toPath()));
            check(s.complete(), s.keysProblem() + " " + s.problem(Setup.BASE) + " " + s.problem(Setup.UPDATE));
            Setup w = new Setup(folder(), new Real());
            w.takeKeys(new ByteArrayInputStream(("header_key = " + FAKE_KEY + FAKE_KEY + "\n").getBytes(StandardCharsets.US_ASCII)));
            w.takeFiles(Collections.singletonList(real(new File(fakes, "base.nsp"))), Setup.BASE);
            same(Setup.OPEN_WRONG, w.problem(Setup.BASE), "another header key");
        });
        test("the reader: damaged files are a plain message, never a crash", () -> {
            Random r = new Random(5);
            Setup s = new Setup(folder(), new Real()); s.takeKeys(Files.newInputStream(keysFile.toPath()));
            File d = folder(), f = new File(d, "damaged");
            byte[][] whole = {Files.readAllBytes(new File(fakes, "base.nsp").toPath()), Files.readAllBytes(new File(fakes, "base.xci").toPath()), Files.readAllBytes(new File(fakes, "update.nsp").toPath())};
            Set<String> seen = new HashSet<>();
            for (int i = 0; i < 1500; i++) {
                byte[] b = whole[i % 3].clone();
                if (i % 5 == 0) b = Arrays.copyOf(b, r.nextInt(b.length));
                else { int head = Math.min(b.length, r.nextBoolean() ? 0x10000 : b.length); for (int k = r.nextInt(40) + 1; k > 0; k--) b[r.nextInt(head)] = (byte) r.nextInt(256); }
                Files.write(f.toPath(), b);
                s.takeFiles(Collections.singletonList(real(f)), i % 2);
                String m = s.problem(i % 2);
                if (m != null) { seen.add(m); check(m.length() <= 200, "long: " + m); check(!m.contains("NCA"), "jargon: " + m); }
            }
            check(seen.size() >= 3, "few different messages: " + seen);
        });
        test("the reader: a file of no game, a folder, an empty file, a pipe-like descriptor", () -> {
            Setup s = new Setup(folder(), new Real()); s.takeKeys(Files.newInputStream(keysFile.toPath()));
            File d = folder(), empty = new File(d, "empty"), big = new File(d, "big");
            Files.write(empty.toPath(), new byte[0]);
            s.takeFiles(Collections.singletonList(real(empty)), Setup.BASE);
            same(Setup.NOT_GAME, s.problem(Setup.BASE), "empty");
            // sparse: nothing is written but the head
            try (RandomAccessFile f = new RandomAccessFile(big, "rw")) { f.setLength(20 * GB); f.write(new byte[64]); }
            long before = bytesRead();
            s.takeFiles(Collections.singletonList(real(big)), Setup.BASE);
            long read = bytesRead() - before;
            same(Setup.NOT_GAME, s.problem(Setup.BASE), "20 GB of nothing");
            check(read < (1 << 20), "read " + read + " bytes of it");
            System.out.println("      20 GB unrelated file: " + read + " bytes read by the process around the call");
            try (RandomAccessFile f = new RandomAccessFile("/dev/zero", "r")) {
                long[] o = new long[6];
                String text = SetupNative.inspect(s.keysFile().getPath(), fd(f), o);
                same("the game file cannot be read as a file", text, "not a regular file"); same((long) Setup.E_IO, o[0], "code");
            }
            big.delete();
        });
    }

    // ---- the player's real files: TOTK_SETUP_REAL (read only, nothing printed from them) ----

    static void realFiles(File dir) {
        File keys = new File(dir, "prod.keys");
        test("real files: the folder is walked, both are found, and only headers are read", () -> {
            Setup s = new Setup(folder(), new Real());
            s.takeKeys(Files.newInputStream(keys.toPath()));
            check(s.keysOk(), "keys: " + s.keysProblem());
            long t0 = System.nanoTime(), r0 = bytesRead();
            s.takeFolder(new RealTree(), dir.getPath(), QUIET, GO);
            long us = (System.nanoTime() - t0) / 1000, read = bytesRead() - r0;
            check(s.complete(), s.keysProblem() + " | " + s.problem(Setup.BASE) + " | " + s.problem(Setup.UPDATE));
            System.out.println("      walk, sort and pair check: " + us + " us, " + read + " bytes read");
            check(read < (32 << 20), "read " + read);
        });
        test("real files: each file by itself, swapped, and the time of one look", () -> {
            Setup s = new Setup(folder(), new Real());
            s.takeKeys(Files.newInputStream(keys.toPath()));
            File base = null, update = null;
            for (File f : dir.listFiles()) if (f.getName().endsWith(".nsp")) { if (f.length() > (8L << 30)) base = f; else update = f; }
            long t0 = System.nanoTime();
            s.takeFiles(Arrays.asList(real(update), real(base)), Setup.UPDATE);
            System.out.println("      two files picked: " + (System.nanoTime() - t0) / 1000 + " us");
            check(s.complete(), s.keysProblem() + " | " + s.problem(Setup.BASE) + " | " + s.problem(Setup.UPDATE));
            same(base.getPath(), s.doc(Setup.BASE).uri, "base"); same(update.getPath(), s.doc(Setup.UPDATE).uri, "update");
            s.takeFiles(Arrays.asList(real(base), real(new File(dir, base.getName()))), Setup.BASE);
            check(s.complete(), "the old pair went");
        });
    }

    public static void main(String[] args) {
        base = new File(args[0], "runs");
        for (File old : base.exists() ? base.listFiles() : new File[0]) delete(old);
        keys(); files(); folders(); words();
        reader(new File(args[0], "fakes"));
        String real = System.getenv("TOTK_SETUP_REAL");
        if (real != null && !real.isEmpty()) realFiles(new File(real));
        for (File old : base.listFiles()) delete(old);
        System.out.println(passed + " passed, " + failed.size() + " failed");
        if (!failed.isEmpty()) { System.out.println(failed); System.exit(1); }
    }

    static void delete(File f) { File[] in = f.listFiles(); if (in != null) for (File c : in) delete(c); f.delete(); }
}

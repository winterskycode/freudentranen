package dev.tok.totk;

import android.content.Context;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Random;

/** IconMod.java on a desktop JVM: the mod made from the real game files through the real PlayerRomfs and merged by Mods.apply,
    then damaged input, which must end in a plain IOException with nothing left behind. tools/icon_test/run.sh. Prints no game
    data and no keys. */
public class IconTest {
    static final Mods.Progress QUIET = what -> { };
    static final String LAYOUT = "UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs", FONT_ARCHIVE = "Font/Font.Nin_NX_NVN.bfarc.zs";
    static final String FONT = "scft/External.bfotf", MASKS = "timg/__Combined.bntx";
    static final String[] FONTS = {FONT_ARCHIVE, "Font/Font_CNzh.Nin_NX_NVN.bfarc.zs", "Font/Font_KRko.Nin_NX_NVN.bfarc.zs", "Font/Font_TWzh.Nin_NX_NVN.bfarc.zs"};
    static int passed;
    static final List<String> failed = new ArrayList<>();
    static byte[] art;
    static List<String> lines;
    static final Map<String, byte[]> game = new HashMap<>();

    interface Test { void run() throws Exception; }

    static void test(String name, Test t) {
        try { t.run(); passed++; System.out.println("ok    " + name); }
        catch (Exception | AssertionError e) { failed.add(name); System.out.println("FAIL  " + name + ": " + e); e.printStackTrace(System.out); }
    }

    static void check(boolean ok, String what) { if (!ok) throw new AssertionError(what); }

    static File folder() { return new File(Mods.ROOT, Mods.ICONS_PLAYSTATION); }

    /** The mod from these files must fail with a message and leave nothing: neither a folder nor the stage. */
    static void refused(String what, Map<String, byte[]> files) throws IOException {
        Mods.deleteTree(folder());
        try { IconMod.make(files::get, art, QUIET); }
        catch (IOException e) {
            check(e.getMessage() != null && !e.getMessage().isEmpty(), what + ": no message");
            System.out.println("      " + what + ": " + e.getMessage());
            String[] left = Mods.ROOT.list();
            check(left == null || Arrays.stream(left).noneMatch(n -> n.equals(Mods.ICONS_PLAYSTATION) || n.startsWith("_making")), what + ": files left: " + Arrays.toString(left));
            return;
        }
        throw new AssertionError(what + ": no error");
    }

    static Map<String, byte[]> with(String path, byte[] data) { Map<String, byte[]> m = new HashMap<>(game); m.put(path, data); return m; }

    /** The archive with these bytes over a member, stored without a dictionary. */
    static byte[] pack(byte[] sarc) throws IOException { return Zstd.compress(sarc, 0); }

    /** Damaged copies of a buffer must give an IOException or a RuntimeException (make turns that into an IOException). */
    interface Patch { void run(byte[] b) throws IOException; }

    static void fuzz(byte[] good, int from, int to, int rounds, long seed, Patch patch) {
        Random r = new Random(seed); int ok = 0, refused = 0;
        for (int i = 0; i < rounds; i++) {
            byte[] b = good.clone();
            for (int n = 1 + r.nextInt(8); n > 0; n--) b[from + r.nextInt(to - from)] = (byte) (r.nextBoolean() ? r.nextInt(256) : r.nextBoolean() ? 0xff : 0);
            if (i % 5 == 0) b = Arrays.copyOf(b, from + r.nextInt(b.length - from));
            try { patch.run(b); ok++; } catch (IOException | RuntimeException e) { refused++; }
        }
        System.out.println("      " + rounds + " damaged copies: " + refused + " refused, " + ok + " still patched");
    }

    public static void main(String[] args) throws Exception {
        File work = new File(args[0]), real = new File(args[1]);
        art = Files.readAllBytes(new File(args[2]).toPath());
        SetupFiles.keys = new File(real, "prod.keys").getPath();
        SetupFiles.base = new File(real, "The Legend of Zelda Tears of the Kingdom.nsp").getPath();
        SetupFiles.update = new File(real, "totk-1.4.3.nsp").getPath();
        File files = new File(work, "files"); Mods.deleteTree(files); files.mkdirs();
        Mods.context(new Context(files));

        test("the mod is made from the player's files", () -> {
            long t0 = System.nanoTime();
            List<String> steps = new ArrayList<>();
            String problem = Mods.makeIcons(art, steps::add);
            System.out.printf("      %.1f s with the mod base, progress: %s%n", (System.nanoTime() - t0) / 1e9, steps);
            check(problem == null, String.valueOf(problem));
            t0 = System.nanoTime();
            problem = Mods.makeIcons(art, QUIET);
            System.out.printf("      %.1f s the second time, the game files already open%n", (System.nanoTime() - t0) / 1e9);
            check(problem == null, String.valueOf(problem));
        });
        Mods.Mod icons = Mods.builtIn(Mods.ICONS_PLAYSTATION);
        test("it is a built-in mod of these drawings", () -> {
            check(icons != null, "no mod");
            check(IconMod.current(icons, art), "not current");
            byte[] other = art.clone(); other[art.length - 1] ^= 1;
            check(!IconMod.current(icons, other), "other drawings count as current");
            Mods.Mod pc = new Mods.Mod(); check(!IconMod.current(pc, art), "a mod with no version counts as current");
            check(!IconMod.current(null, art), "no mod counts as current");
            check(icons.replaced == 5 && icons.contents.size() == 2, "mod.json");
            lines = Mods.lines(new File(folder(), "files.tsv"));
            check(lines.size() == 5, "files.tsv");
            check(Mods.list().isEmpty(), "a built-in mod on the Mods page");
        });
        test("Mods.apply merges it", () -> {
            List<Mods.Mod> on = new ArrayList<>(); on.add(icons);
            check(!Mods.upToDate(on), "up to date before");
            Mods.apply(on, QUIET);
            check(Mods.upToDate(on), "not up to date after");
            List<String> manifest = Mods.lines(new File(Mods.ACTIVE, "manifest.tsv"));
            check(manifest.size() == 6, manifest.size() + " lines");
            for (String l : manifest) {
                String[] f = l.split("\t");
                File source = f[4].startsWith("/") ? new File(f[4]) : new File(Mods.ACTIVE, f[4]);
                check(source.length() == Long.parseLong(f[1]), f[0] + ": size");
                check(ModData.hex(ModData.sha256(Files.readAllBytes(source.toPath()))).equals(f[2]), f[0] + ": hash");
                check(f[3].equals(PlayerRomfs.sha(f[0])), f[0] + ": original");
            }
            ModData.Restbl original = ModData.Restbl.read(Mods.unzs(PlayerRomfs.read(Mods.RSTB)));
            ModData.Restbl table = ModData.Restbl.read(Mods.unzs(Files.readAllBytes(new File(Mods.ACTIVE, "gen/" + Mods.RSTB).toPath())));
            for (String l : Mods.lines(new File(folder(), "files.tsv"))) {
                Mods.Entry e = Mods.Entry.parse(l);
                check(table.get(e.path) == e.hint, e.path + ": table " + table.get(e.path) + ", hint " + e.hint);
                check(e.hint >= original.get(e.path), e.path + ": less room than the game's");
                System.out.println("      " + e.path + ": +" + (e.hint - original.get(e.path)));
            }
        });

        for (String p : FONTS) game.put(p, PlayerRomfs.read(p));
        game.put(LAYOUT, PlayerRomfs.read(LAYOUT));
        game.put(Mods.RSTB, PlayerRomfs.read(Mods.RSTB));
        byte[] fontArchive = Zstd.decompress(game.get(FONT_ARCHIVE)), layout = Zstd.decompress(game.get(LAYOUT));
        int[] fontAt = ModData.member(fontArchive, FONT), masksAt = ModData.member(layout, MASKS);
        IconMod.Bfotf font = IconMod.decode(Arrays.copyOfRange(fontArchive, fontAt[0], fontAt[1]));
        IconMod.Art drawn = IconMod.Art.read(art);
        System.out.println("      font " + font.otf.length + " bytes, masks member " + (masksAt[1] - masksAt[0]) + " bytes");

        test("damaged drawings", () -> {
            for (byte[] bad : new byte[][]{null, new byte[0], "TKICONS1".getBytes(), Arrays.copyOf(art, art.length - 1), Arrays.copyOf(art, art.length + 1)}) {
                try { IconMod.Art.read(bad); throw new AssertionError("no error"); } catch (IOException e) { check(e.getMessage() != null, "message"); }
            }
            check(Mods.makeIcons(null, QUIET) != null, "makeIcons with no drawings");
            System.out.println("      " + Mods.makeIcons(null, QUIET));
        });
        test("a game file missing", () -> {
            for (String p : game.keySet()) { Map<String, byte[]> m = new HashMap<>(game); m.remove(p); refused("no " + p, m); }
        });
        test("a game file cut short or not zstd", () -> {
            for (String p : game.keySet()) {
                byte[] raw = game.get(p);
                refused("half of " + p, with(p, Arrays.copyOf(raw, raw.length / 2)));
                if (!p.equals(Mods.RSTB)) refused("nonsense as " + p, with(p, new byte[4096]));
            }
        });
        test("an archive that is not one", () -> {
            refused("junk font archive", with(FONT_ARCHIVE, pack(new byte[4096])));
            byte[] noise = new byte[1 << 16]; new Random(1).nextBytes(noise); noise[0] = 'S'; noise[1] = 'A'; noise[2] = 'R'; noise[3] = 'C';
            refused("noise font archive", with(FONT_ARCHIVE, pack(noise)));
            refused("noise layout archive", with(LAYOUT, pack(noise)));
            refused("junk size table", with(Mods.RSTB, pack(new byte[64])));
        });
        test("an archive with no font or no textures", () -> {
            byte[] b = fontArchive.clone(); int name = indexOf(b, FONT.getBytes()); b[name] = 'x';
            refused("font renamed", with(FONT_ARCHIVE, pack(b)));
            b = layout.clone(); name = indexOf(b, MASKS.getBytes()); b[name] = 'x';
            refused("textures renamed", with(LAYOUT, pack(b)));
        });
        test("a damaged font in its archive", () -> {
            byte[] b = fontArchive.clone(); b[fontAt[0]] ^= 0x5a;
            refused("wrong key", with(FONT_ARCHIVE, pack(b)));
            b = fontArchive.clone(); Arrays.fill(b, fontAt[0] + 8, fontAt[1], (byte) 0);
            refused("blanked", with(FONT_ARCHIVE, pack(b)));
        });
        test("textures that are not the known ones", () -> {
            byte[] b = layout.clone(); int brti = textureHeader(b, masksAt, "Nt_KeyTexZL_00");
            b[masksAt[0] + (int) ModData.le(b).getLong(brti + 0x60) + 2] = 'x';
            refused("a texture renamed", with(LAYOUT, pack(b)));
            b = layout.clone(); brti = textureHeader(b, masksAt, "Nt_KeyTexA_00"); ModData.le(b).putInt(brti + 0x1c, 0x101);
            refused("another format", with(LAYOUT, pack(b)));
            b = layout.clone(); ModData.le(b).putInt(brti + 0x24, 72);
            refused("another size", with(LAYOUT, pack(b)));
            b = layout.clone(); ModData.le(b).putLong(brti + 0x70, Long.MAX_VALUE);
            refused("its data pointer gone", with(LAYOUT, pack(b)));
        });
        test("damaged fonts: never anything but an exception", () ->
            fuzz(font.otf, 0, font.otf.length, 20000, 1, b -> IconMod.patchFont(b, drawn)));
        test("damaged font headers", () -> fuzz(font.otf, 0, 2100, 20000, 2, b -> IconMod.patchFont(b, drawn)));
        test("damaged .bfotf headers", () -> fuzz(Arrays.copyOfRange(fontArchive, fontAt[0], fontAt[1]), 0, 16, 2000, 3, IconMod::decode));
        test("damaged SARC headers", () -> {
            int head = ModData.le(fontArchive).getInt(12);
            fuzz(fontArchive, 0, head, 500, 4, b -> ModData.replaceMember(b, FONT, new byte[100]));
        });
        test("damaged texture headers", () -> {
            int first = textureHeader(layout, masksAt, "Nt_KeyTexA_00");
            byte[] bntx = Arrays.copyOfRange(layout, masksAt[0], masksAt[1]); first -= masksAt[0];
            byte[] clean = bntx.clone(); Random r = new Random(5); int refused = 0, ok = 0;
            for (int i = 0; i < 3000; i++) {
                int at = first + r.nextInt(0x1000); byte was = bntx[at]; bntx[at] = (byte) r.nextInt(256);
                try { IconMod.patchMasks(bntx, 0, bntx.length, drawn); ok++; } catch (IOException | RuntimeException e) { refused++; }
                bntx[at] = was;
                if (i % 500 == 0) System.arraycopy(clean, 0, bntx, 0, bntx.length);
            }
            System.out.println("      3000 damaged copies: " + refused + " refused, " + ok + " still patched");
        });
        Mods.deleteTree(folder());
        test("made again after all that, the same bytes", () -> {
            check(Mods.makeIcons(art, QUIET) == null, "failed");
            check(Mods.builtIn(Mods.ICONS_PLAYSTATION) != null, "no mod");
            check(Mods.lines(new File(folder(), "files.tsv")).equals(lines), "files.tsv differs");
        });

        System.out.println(passed + " passed, " + failed.size() + " failed " + failed);
        if (!failed.isEmpty()) System.exit(1);
    }

    static int indexOf(byte[] b, byte[] what) {
        outer: for (int i = 0; i + what.length <= b.length; i++) { for (int k = 0; k < what.length; k++) if (b[i + k] != what[k]) continue outer; return i; }
        throw new AssertionError("not found");
    }

    /** The BRTI of a texture, in the archive. */
    static int textureHeader(byte[] b, int[] at, String name) {
        java.nio.ByteBuffer d = ModData.le(b); byte[] want = name.getBytes();
        for (int i = at[0]; i < at[1] - 0x78; i++) {
            if (b[i] != 'B' || b[i + 1] != 'R' || b[i + 2] != 'T' || b[i + 3] != 'I') continue;
            long nameAt = d.getLong(i + 0x60); if (nameAt < 0 || nameAt > at[1] - at[0] - 2) continue;
            int start = at[0] + (int) nameAt + 2; boolean same = true;
            for (int k = 0; k < want.length && same; k++) same = b[start + k] == want[k];
            if (same) return i;
        }
        throw new AssertionError("no texture " + name);
    }
}

package dev.tok.totk;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

/** The built-in mod of the PlayStation button icons (Mods.ICONS_PLAYSTATION), made once on the device from the player's own
    files and the drawings in the APK (assets/icons/playstation.bin, tools/make_icon_asset.py). It redraws glyphs of the
    game's icon font (scft/External.bfotf, in each font archive) and the eight button-guide masks (timg/__Combined.bntx of the
    common layout archive). The result is the folder a mod has: romfs/, files.tsv, mod.json. Mods.apply takes it from there and
    the size table gets each archive's growth from files.tsv. Everything not redrawn stays byte for byte. The same edits as
    MacNativeGames' scripts/make_button_icons.py, except that the font is edited in place instead of being saved anew. */
final class IconMod {
    private IconMod() {}

    static final String ASSET = "icons/playstation.bin";
    private static final String STAGE = "_making-icons", FONT = "scft/External.bfotf", MASKS = "timg/__Combined.bntx";
    private static final String LAYOUT = "UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs";
    /** Largest first, so that the longest job does not start last. */
    private static final String[] ARCHIVES = {LAYOUT, "Font/Font_TWzh.Nin_NX_NVN.bfarc.zs", "Font/Font_CNzh.Nin_NX_NVN.bfarc.zs",
        "Font/Font_KRko.Nin_NX_NVN.bfarc.zs", "Font/Font.Nin_NX_NVN.bfarc.zs"};
    private static final int BFOTF_KEY = 0x18029a7f, R8_UNORM = 0x201, CHECKSUM_TOTAL = 0xb1b0afba;
    /** Two archives at a time: three needed all of a 256 MB heap, a usual limit for an app (tools/icon_test with -Xmx). */
    private static final int THREADS = 2;

    interface Files { byte[] read(String path) throws IOException; }

    // ---- the drawings ----

    static final class Glyph {
        final int code, advance, bearing; final byte[] outline;
        Glyph(int code, int advance, int bearing, byte[] outline) { this.code = code; this.advance = advance; this.bearing = bearing; this.outline = outline; }
    }

    static final class Art {
        final List<Glyph> glyphs = new ArrayList<>();
        final Map<String, byte[]> masks = new LinkedHashMap<>();
        int size;
        String id = "";   // in a mod's mod.json as its version: another APK's drawings make the mod again

        static Art read(byte[] b) throws IOException {
            if (b == null) throw new IOException("This app has no drawings of them.");
            try {
                ByteBuffer d = ModData.le(b);
                byte[] magic = new byte[8]; d.get(magic);
                if (!new String(magic, StandardCharsets.US_ASCII).equals("TKICONS1")) throw new IOException("The drawings in this app cannot be read.");
                Art a = new Art(); a.id = ModData.hex(ModData.sha256(b)).substring(0, 16);
                int glyphs = d.getShort() & 0xffff, masks = d.getShort() & 0xffff;
                for (int i = 0; i < glyphs; i++) {
                    int code = d.getShort() & 0xffff, advance = d.getShort() & 0xffff, bearing = d.getShort();
                    byte[] outline = new byte[d.getShort() & 0xffff]; d.get(outline);
                    a.glyphs.add(new Glyph(code, advance, bearing, outline));
                }
                for (int i = 0; i < masks; i++) {
                    int size = d.getShort() & 0xffff; byte[] name = new byte[d.get() & 0xff]; d.get(name);
                    if (a.size != 0 && a.size != size) throw new IOException("The drawings in this app cannot be read.");
                    a.size = size;
                    byte[] mask = new byte[size * size]; d.get(mask);
                    a.masks.put(new String(name, StandardCharsets.US_ASCII), mask);
                }
                if (d.hasRemaining()) throw new IOException("The drawings in this app cannot be read.");
                return a;
            } catch (RuntimeException e) { throw new IOException("The drawings in this app cannot be read."); }
        }
    }

    /** Whether this folder is what these drawings make (a mod from the PC has no version). */
    static boolean current(Mods.Mod m, byte[] asset) throws IOException { return m != null && m.version.equals(Art.read(asset).id); }

    // ---- .bfotf: an OpenType font behind an 8-byte header, XOR-ed with a 32-bit key ----

    static final class Bfotf {
        final byte[] otf; final int key;
        Bfotf(byte[] otf, int key) { this.otf = otf; this.key = key; }
    }

    private static void xor(byte[] from, int at, byte[] to, int into, int size, int key) {
        ByteBuffer a = ModData.le(from), b = ModData.le(to); int whole = size & ~3;
        for (int i = 0; i < whole; i += 4) b.putInt(into + i, a.getInt(at + i) ^ key);
        System.arraycopy(from, at + whole, to, into + whole, size - whole);
    }

    static Bfotf decode(byte[] blob) throws IOException {
        ByteBuffer b = ModData.le(blob); int key = b.getInt(0) ^ BFOTF_KEY, size = Integer.reverseBytes(b.getInt(4) ^ key);
        if (size < 12 || size > blob.length - 8) throw new IOException("The game's icon font cannot be read.");
        byte[] otf = new byte[size]; xor(blob, 8, otf, 0, size, key);
        if (otf[0] != 'O' || otf[1] != 'T' || otf[2] != 'T' || otf[3] != 'O') throw new IOException("The game's icon font cannot be read.");
        return new Bfotf(otf, key);
    }

    static byte[] encode(byte[] otf, int key) {
        byte[] blob = new byte[8 + otf.length]; ByteBuffer b = ModData.le(blob);
        b.putInt(0, BFOTF_KEY ^ key); b.putInt(4, Integer.reverseBytes(otf.length) ^ key);
        xor(otf, 0, blob, 8, otf.length, key);
        return blob;
    }

    // ---- the font ----

    private static int u8(byte[] b, int at) { return b[at] & 0xff; }
    private static int u16(byte[] b, int at) { return u8(b, at) << 8 | u8(b, at + 1); }
    private static int i32(byte[] b, int at) { return u16(b, at) << 16 | u16(b, at + 2); }
    private static void put16(byte[] b, int at, int v) { b[at] = (byte) (v >> 8); b[at + 1] = (byte) v; }
    private static void put32(byte[] b, int at, int v) { put16(b, at, v >>> 16); put16(b, at + 2, v); }
    private static int align4(int n) { return (n + 3) & ~3; }

    /** The place in the table directory of a table with this tag. */
    private static int entry(byte[] otf, String tag) throws IOException {
        byte[] t = tag.getBytes(StandardCharsets.US_ASCII);
        for (int i = 0, n = u16(otf, 4); i < n; i++) {
            int e = 12 + 16 * i;
            if (Arrays.equals(Arrays.copyOfRange(otf, e, e + 4), t)) {
                int at = i32(otf, e + 8), size = i32(otf, e + 12);
                if (at < 0 || size < 0 || at > otf.length - size) break;
                return e;
            }
        }
        throw new IOException("The game's icon font has no usable " + tag.trim() + " table.");
    }

    /** The glyph of a code point in a format 4 subtable, 0 if none. */
    private static int glyphOf(byte[] otf, int table, int code) {
        int segments = u16(otf, table + 6) / 2, ends = table + 14, starts = ends + 2 * segments + 2, deltas = starts + 2 * segments, ranges = deltas + 2 * segments;
        for (int i = 0; i < segments; i++) {
            if (u16(otf, ends + 2 * i) < code) continue;
            int start = u16(otf, starts + 2 * i), delta = u16(otf, deltas + 2 * i), range = u16(otf, ranges + 2 * i);
            if (start > code) return 0;
            if (range == 0) return (code + delta) & 0xffff;
            int g = u16(otf, ranges + 2 * i + range + 2 * (code - start));
            return g == 0 ? 0 : (g + delta) & 0xffff;
        }
        return 0;
    }

    /** The Unicode subtable of the cmap (Windows BMP or Unicode BMP). */
    private static int unicodeCmap(byte[] otf) throws IOException {
        int cmap = i32(otf, entry(otf, "cmap") + 8);
        for (int i = 0, n = u16(otf, cmap + 2); i < n; i++) {
            int e = cmap + 4 + 8 * i, platform = u16(otf, e), encoding = u16(otf, e + 2), table = cmap + i32(otf, e + 4);
            if (((platform == 3 && encoding == 1) || (platform == 0 && encoding == 3)) && u16(otf, table) == 4) return table;
        }
        throw new IOException("The game's icon font has no usable cmap table.");
    }

    /** A CFF INDEX: count entries of offSize-byte offsets, then their data. */
    private static final class Index {
        final byte[] b; final int start, count, offSize, data, end;
        Index(byte[] b, int at) throws IOException {
            this.b = b; start = at; count = u16(b, at);
            if (count == 0) { offSize = 1; data = end = at + 2; return; }
            offSize = u8(b, at + 2);
            if (offSize < 1 || offSize > 4) throw new IOException("The game's icon font has a damaged CFF table.");
            data = at + 3 + (count + 1) * offSize - 1; end = data + offset(count);
            if (end < data || end > b.length) throw new IOException("The game's icon font has a damaged CFF table.");
        }
        private int offset(int i) { int v = 0; for (int k = 0; k < offSize; k++) v = v << 8 | u8(b, start + 3 + i * offSize + k); return v; }
        int from(int i) { return data + offset(i); }
        int to(int i) { return data + offset(i + 1); }
    }

    private static final int REAL = Integer.MIN_VALUE;

    /** A CFF DICT: per operator (two-byte ones as 1200 + the second) its operands as {value, place, width}. */
    private static Map<Integer, List<int[]>> dict(byte[] b, int at, int end) throws IOException {
        Map<Integer, List<int[]>> out = new HashMap<>(); List<int[]> args = new ArrayList<>();
        while (at < end) {
            int v = u8(b, at);
            if (v <= 21) { int op = v == 12 ? 1200 + u8(b, at + 1) : v; at += v == 12 ? 2 : 1; out.put(op, args); args = new ArrayList<>(); }
            else if (v == 28) { args.add(new int[]{(short) u16(b, at + 1), at, 3}); at += 3; }
            else if (v == 29) { args.add(new int[]{i32(b, at + 1), at, 5}); at += 5; }
            else if (v == 30) { int from = at, x; do { x = u8(b, ++at); } while ((x & 0xf) != 0xf && x >> 4 != 0xf); at++; args.add(new int[]{REAL, from, at - from}); }
            else if (v >= 32 && v <= 246) { args.add(new int[]{v - 139, at, 1}); at++; }
            else if (v >= 247 && v <= 250) { args.add(new int[]{(v - 247) * 256 + u8(b, at + 1) + 108, at, 2}); at += 2; }
            else if (v >= 251 && v <= 254) { args.add(new int[]{-(v - 251) * 256 - u8(b, at + 1) - 108, at, 2}); at += 2; }
            else throw new IOException("The game's icon font has a damaged CFF table.");
        }
        return out;
    }

    /** An operand moved by the bytes the CharStrings grew, in the width it has: the DICT keeps its size. */
    private static void shift(byte[] cff, int[] operand, int moved) throws IOException {
        int v = operand[0] + moved, at = operand[1];
        if (operand[2] == 3 && v <= Short.MAX_VALUE) put16(cff, at + 1, v);
        else if (operand[2] == 5) put32(cff, at + 1, v);
        else if (v != operand[0]) throw new IOException("The game's icon font has no room for the new icons.");
    }

    /** A width as a Type 2 number. */
    private static byte[] number(int v) {
        if (v >= -107 && v <= 107) return new byte[]{(byte) (v + 139)};
        if (v >= 108 && v <= 1131) return new byte[]{(byte) (247 + ((v - 108) >> 8)), (byte) (v - 108)};
        if (v >= -1131 && v <= -108) return new byte[]{(byte) (251 + ((-v - 108) >> 8)), (byte) (-v - 108)};
        return new byte[]{28, (byte) (v >> 8), (byte) v};
    }

    /** The CFF table with these charstrings (glyph -> its new bytes); top is its Top DICT. */
    private static byte[] charStrings(byte[] cff, Map<Integer, List<int[]>> top, Map<Integer, byte[]> outlines, int glyphs) throws IOException {
        if (top.containsKey(1230) || !top.containsKey(17) || top.get(17).size() != 1) throw new IOException("The game's icon font is not of the known kind.");
        Index strings = new Index(cff, top.get(17).get(0)[0]);
        if (strings.count != glyphs) throw new IOException("The game's icon font has a damaged CFF table.");

        int[] offsets = new int[glyphs + 1]; int size = 1;
        for (int g = 0; g < glyphs; g++) {
            byte[] mine = outlines.get(g); offsets[g] = size;
            if (strings.from(g) > strings.to(g) || strings.to(g) > strings.end) throw new IOException("The game's icon font has a damaged CFF table.");
            size += mine != null ? mine.length : strings.to(g) - strings.from(g);
        }
        offsets[glyphs] = size;
        int offSize = strings.offSize; while (offSize < 4 && size >>> (8 * offSize) != 0) offSize++;
        int head = 3 + (glyphs + 1) * offSize, length = head + size - 1, moved = strings.start + length - strings.end;
        byte[] out = new byte[cff.length + moved];
        System.arraycopy(cff, 0, out, 0, strings.start);
        put16(out, strings.start, glyphs); out[strings.start + 2] = (byte) offSize;
        for (int g = 0; g <= glyphs; g++) for (int k = 0; k < offSize; k++) out[strings.start + 3 + g * offSize + k] = (byte) (offsets[g] >>> (8 * (offSize - 1 - k)));
        int data = strings.start + head - 1;
        for (int g = 0; g < glyphs; g++) {
            byte[] mine = outlines.get(g);
            if (mine != null) System.arraycopy(mine, 0, out, data + offsets[g], mine.length);
            else System.arraycopy(cff, strings.from(g), out, data + offsets[g], strings.to(g) - strings.from(g));
        }
        System.arraycopy(cff, strings.end, out, strings.start + length, cff.length - strings.end);
        for (int op : new int[]{15, 16, 18}) {   // charset, Encoding, Private: what lies after the charstrings moves with them
            List<int[]> args = top.get(op); if (args == null) continue;
            int[] offset = args.get(args.size() - 1);
            if (offset[0] >= strings.end) shift(out, offset, moved);
        }
        return out;
    }

    private static int checksum(byte[] b, int at, int size) {
        int sum = 0, whole = size & ~3;
        for (int i = 0; i < whole; i += 4) sum += i32(b, at + i);
        if (whole < size) { byte[] tail = new byte[4]; System.arraycopy(b, at + whole, tail, 0, size - whole); sum += i32(tail, 0); }
        return sum;
    }

    /** The font with the drawn glyphs: their charstrings, advances and side bearings. The rest stays, the tables in their order:
        the head's and hhea's bounds too, as the drawings lie inside them (tools/icon_test/compare.py checks that). */
    static byte[] patchFont(byte[] otf, Art art) throws IOException {
        int cffEntry = entry(otf, "CFF "), hmtxEntry = entry(otf, "hmtx"), headEntry = entry(otf, "head");
        int cffAt = i32(otf, cffEntry + 8), cffSize = i32(otf, cffEntry + 12), glyphs = u16(otf, i32(otf, entry(otf, "maxp") + 8) + 4);
        int metrics = u16(otf, i32(otf, entry(otf, "hhea") + 8) + 34), cmap = unicodeCmap(otf);
        byte[] cff = Arrays.copyOfRange(otf, cffAt, cffAt + cffSize);

        Index name = new Index(cff, u8(cff, 2)), topIndex = new Index(cff, name.end);
        if (topIndex.count != 1) throw new IOException("The game's icon font has a damaged CFF table.");
        Map<Integer, List<int[]>> top = dict(cff, topIndex.from(0), topIndex.to(0));
        List<int[]> privateDict = top.get(18);
        if (privateDict == null || privateDict.size() != 2) throw new IOException("The game's icon font has a damaged CFF table.");
        int privateSize = privateDict.get(0)[0], privateAt = privateDict.get(1)[0];
        if (privateSize < 0 || privateAt < 0 || privateAt > cff.length - privateSize) throw new IOException("The game's icon font has a damaged CFF table.");
        List<int[]> nominalWidth = dict(cff, privateAt, privateAt + privateSize).get(21);
        int nominal = nominalWidth == null || nominalWidth.isEmpty() ? 0 : nominalWidth.get(0)[0];
        if (nominal == REAL) throw new IOException("The game's icon font is not of the known kind.");

        Map<Integer, byte[]> outlines = new HashMap<>(); Map<Integer, Glyph> drawn = new HashMap<>();
        for (Glyph g : art.glyphs) {
            int id = glyphOf(otf, cmap, g.code);
            if (id == 0 || id >= glyphs || id >= metrics) throw new IOException("The game's icon font lacks an icon.");
            byte[] width = number(g.advance - nominal), both = Arrays.copyOf(width, width.length + g.outline.length);
            System.arraycopy(g.outline, 0, both, width.length, g.outline.length);
            outlines.put(id, both); drawn.put(id, g);
        }
        byte[] newCff = charStrings(cff, top, outlines, glyphs);

        int oldEnd = align4(cffAt + cffSize), newEnd = align4(cffAt + newCff.length), moved = newEnd - oldEnd;
        if (oldEnd > otf.length) throw new IOException("The game's icon font has a damaged CFF table.");
        byte[] out = new byte[otf.length + moved];
        System.arraycopy(otf, 0, out, 0, cffAt);
        System.arraycopy(newCff, 0, out, cffAt, newCff.length);
        System.arraycopy(otf, oldEnd, out, newEnd, otf.length - oldEnd);
        for (int i = 0, n = u16(out, 4); i < n; i++) { int e = 12 + 16 * i, at = i32(out, e + 8); if (at >= oldEnd) put32(out, e + 8, at + moved); }
        put32(out, cffEntry + 12, newCff.length);

        int hmtx = i32(out, hmtxEntry + 8), head = i32(out, headEntry + 8), hmtxSize = i32(out, hmtxEntry + 12), headSize = i32(out, headEntry + 12);
        if (hmtxSize < 4 * metrics || headSize < 54) throw new IOException("The game's icon font has a damaged metrics table.");
        for (Map.Entry<Integer, Glyph> e : drawn.entrySet()) { put16(out, hmtx + 4 * e.getKey(), e.getValue().advance); put16(out, hmtx + 4 * e.getKey() + 2, e.getValue().bearing); }
        put32(out, head + 8, 0);
        put32(out, cffEntry + 4, checksum(out, cffAt, newCff.length));
        put32(out, hmtxEntry + 4, checksum(out, hmtx, hmtxSize));
        put32(out, headEntry + 4, checksum(out, head, headSize));
        put32(out, head + 8, CHECKSUM_TOTAL - checksum(out, 0, out.length));
        return out;
    }

    // ---- the button guide's masks ----

    /** Byte offset of (x, y) in a Tegra block-linear surface of 1 byte per pixel (GOBs of 64 bytes x 8 rows). */
    static int gobOffset(int x, int y, int widthGobs, int blockHeight) {
        return (y / (8 * blockHeight)) * 512 * blockHeight * widthGobs + (x / 64) * 512 * blockHeight
            + ((y % (8 * blockHeight)) / 8) * 512 + ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64
            + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
    }

    /** Writes the masks over the textures of that name in the BNTX at [at, end) of b: nothing else in it changes. */
    static void patchMasks(byte[] b, int at, int end, Art art) throws IOException {
        ByteBuffer d = ModData.le(b); Set<String> left = new HashSet<>(art.masks.keySet());
        for (int i = at; i <= end - 0x78; i++) {
            if (b[i] != 'B' || b[i + 1] != 'R' || b[i + 2] != 'T' || b[i + 3] != 'I') continue;
            long nameAt = d.getLong(i + 0x60); if (nameAt < 0 || nameAt > end - at - 2) continue;   // those bytes in a picture: no texture
            int nameSize = d.getShort(at + (int) nameAt) & 0xffff; if (nameAt + 2 + nameSize > end - at) continue;
            String name = new String(b, at + (int) nameAt + 2, nameSize, StandardCharsets.ISO_8859_1);
            int hat = name.indexOf('^'); if (hat >= 0) name = name.substring(0, hat);
            byte[] mask = art.masks.get(name); if (mask == null) continue;
            int mips = d.getShort(i + 0x16) & 0xffff, format = d.getInt(i + 0x1c), width = d.getInt(i + 0x24), height = d.getInt(i + 0x28), layout = d.getInt(i + 0x34);
            if (format != R8_UNORM || width != art.size || height != art.size || mips != 1) throw new IOException("A button texture of the game is not of the known kind.");
            long table = d.getLong(i + 0x70); if (table < 0 || table > end - at - 8) throw new IOException("A button texture of the game is damaged.");
            long data = d.getLong(at + (int) table);
            int widthGobs = (width + 63) / 64, blockHeight = 1 << (layout & 7);
            if (data < 0 || data + gobOffset(width - 1, height - 1, widthGobs, blockHeight) >= end - at) throw new IOException("A button texture of the game is damaged.");
            for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) b[at + (int) data + gobOffset(x, y, widthGobs, blockHeight)] = mask[y * width + x];
            left.remove(name);
        }
        if (!left.isEmpty()) throw new IOException("A button texture of the game is missing.");
    }

    // ---- the mod ----

    /** One archive of the game, its drawn copy written under dir: its files.tsv line, with the size table's room for it. */
    private static Mods.Entry archive(Files game, String path, File dir, Art art, ModData.Restbl table) throws IOException {
        byte[] raw = game.read(path);
        if (raw == null) throw new IOException("The game files lack " + path.substring(path.lastIndexOf('/') + 1) + ".");
        int dictionary = Zstd.dictionaryOf(raw);
        byte[] sarc = Zstd.decompress(raw), drawn = sarc;
        if (path.equals(LAYOUT)) {
            int[] at = ModData.member(sarc, MASKS);
            if (at == null) throw new IOException("The button textures of the game are missing.");
            patchMasks(sarc, at[0], at[1], art);
        } else {
            int[] at = ModData.member(sarc, FONT);
            if (at == null) throw new IOException("The game's icon font is missing.");
            Bfotf font = decode(Arrays.copyOfRange(sarc, at[0], at[1]));
            drawn = ModData.replaceMember(sarc, FONT, encode(patchFont(font.otf, art), font.key));
        }
        byte[] packed = Zstd.compress(drawn, dictionary);
        File file = new File(dir, "romfs/" + path); file.getParentFile().mkdirs();
        Mods.writeBytes(file, packed);
        Mods.Entry e = new Mods.Entry();
        e.path = path; e.size = packed.length; e.sha = ModData.hex(ModData.sha256(packed)); e.orig = ModData.hex(ModData.sha256(raw));
        e.kind = "copy"; e.hint = table.get(path) + drawn.length - sarc.length;
        return e;
    }

    private static void files(Files game, File dir, Art art, Mods.Progress p, List<Mods.Entry> entries) throws IOException, InterruptedException, ExecutionException {
        byte[] rawTable = game.read(Mods.RSTB);
        if (rawTable == null) throw new IOException("The game files lack the size table.");
        ModData.Restbl table = ModData.Restbl.read(Mods.unzs(rawTable));
        ExecutorService pool = Executors.newFixedThreadPool(THREADS);
        try {
            List<Future<Mods.Entry>> jobs = new ArrayList<>();
            for (String path : ARCHIVES) jobs.add(pool.submit(() -> archive(game, path, dir, art, table)));
            int done = 0;
            for (Future<Mods.Entry> job : jobs) { entries.add(job.get()); p.step(String.format(Locale.ROOT, "Making the PlayStation buttons %d / %d", ++done, ARCHIVES.length)); }
            entries.sort(Comparator.comparing(e -> e.path));
        } finally {   // after a failure too: a job still running would write into the folder that is being removed
            pool.shutdownNow(); pool.awaitTermination(5, TimeUnit.MINUTES);
        }
    }

    /** Makes the mod's folder in Mods.ROOT, in one step at the end: a folder there is whole. Bad input is an IOException. */
    static void make(Files game, byte[] asset, Mods.Progress p) throws IOException {
        Art art = Art.read(asset);
        File stage = new File(Mods.ROOT, STAGE), dir = new File(Mods.ROOT, Mods.ICONS_PLAYSTATION);
        Mods.deleteTree(stage); stage.mkdirs();
        try {
            List<Mods.Entry> entries = new ArrayList<>();
            try { files(game, stage, art, p, entries); }
            catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new IOException("It was stopped."); }
            catch (ExecutionException e) {
                if (e.getCause() instanceof IOException) throw (IOException) e.getCause();
                if (e.getCause() instanceof OutOfMemoryError) throw new IOException("The app ran out of memory.");
                throw new IOException("The game files are not as expected.");
            }
            catch (RuntimeException e) { throw new IOException("The game files are not as expected."); }
            StringBuilder tsv = new StringBuilder(); Mods.Mod m = new Mods.Mod(); m.id = STAGE;
            for (Mods.Entry e : entries) { tsv.append(e.line()).append('\n'); m.bytes += e.size; }
            Mods.write(new File(stage, "files.tsv"), tsv.toString());
            m.name = "PlayStation button icons"; m.author = "This app"; m.version = art.id; m.source = "built in"; m.made = "1.4.3";
            m.description = "Built in: on while Settings > Controls > Button letters is PlayStation.";
            m.installed = System.currentTimeMillis() / 1000; m.replaced = entries.size();
            m.contents.add("Font archives"); m.contents.add("Button guide textures");
            Mods.writeMod(m);
            Mods.deleteTree(dir);
            if (!stage.renameTo(dir) || Mods.builtIn(Mods.ICONS_PLAYSTATION) == null) throw new IOException("Its folder cannot be written.");
        } finally { Mods.deleteTree(stage); }
    }
}

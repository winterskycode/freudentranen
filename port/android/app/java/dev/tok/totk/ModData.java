package dev.tok.totk;

import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.zip.CRC32;

/** The game formats the mod installer reads and writes (Mods.java): the RomFS image, SARC packs, BYML documents (only to
    compare them), the resource size table (RESTBL) and the 1.0.0 base (tools/make_mod_base.py). The same rules as
    MacNativeGames' scripts/port_mod.py, whose writers reproduce 1.4.3's own packs and table byte for byte. */
final class ModData {
    private ModData() {}

    // ---- hashes ----

    static byte[] sha256(byte[] b) { return digest().digest(b); }
    static MessageDigest digest() {
        try { return MessageDigest.getInstance("SHA-256"); } catch (NoSuchAlgorithmException e) { throw new IllegalStateException(e); }
    }
    static String hex(byte[] b) { StringBuilder s = new StringBuilder(); for (byte x : b) s.append(String.format("%02x", x & 0xff)); return s.toString(); }
    /** The first 8 bytes of SHA-256, big endian: the base file's keys and hashes (so unsigned order is byte order). */
    static long hash8(byte[] b) { return ByteBuffer.wrap(sha256(b)).getLong(); }
    static long key8(String path) { return hash8(path.getBytes(StandardCharsets.UTF_8)); }

    static ByteBuffer le(byte[] b) { return ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN); }

    // ---- the RomFS image: runtime-index.bin (sorted names) and romfs.bin ----

    static final class Romfs implements Closeable {
        private final RandomAccessFile indexFile, image;
        private final ByteBuffer index;
        private final long count, strings;
        final boolean player;   // through PlayerRomfs: the player's own files, no image

        Romfs() { indexFile = image = null; index = null; count = strings = 0; player = true; }

        Romfs(File dir) throws IOException {
            player = false;
            indexFile = new RandomAccessFile(new File(dir, "runtime-index.bin"), "r");
            index = indexFile.getChannel().map(FileChannel.MapMode.READ_ONLY, 0, indexFile.length());
            index.order(ByteOrder.LITTLE_ENDIAN);
            byte[] magic = new byte[8]; index.duplicate().get(magic, 0, 8);   // not get(int, byte[], ...): that is Android 14 and up
            if (!new String(magic, StandardCharsets.US_ASCII).equals("TOTKRFS1")) throw new IOException("not a RomFS index");
            count = index.getLong(8); strings = index.getLong(16);
            image = new RandomAccessFile(new File(dir, "romfs.bin"), "r");
        }

        private int compareName(long i, byte[] want) {
            long e = 24 + i * 24; int name = index.getInt((int) (e + 16)), len = index.getInt((int) (e + 20)) - 1;
            int base = (int) (strings + (name & 0xffffffffL));
            for (int k = 0; k < Math.min(len, want.length); k++) {
                int a = index.get(base + k) & 0xff, b = want[k] & 0xff;
                if (a != b) return a - b;
            }
            return len - want.length;
        }

        /** The entry of path, or -1. */
        long find(String path) {
            byte[] want = path.getBytes(StandardCharsets.UTF_8);
            long lo = 0, hi = count;
            while (lo < hi) { long m = (lo + hi) >>> 1; if (compareName(m, want) < 0) lo = m + 1; else hi = m; }
            return lo < count && compareName(lo, want) == 0 ? lo : -1;
        }

        boolean has(String path) { return player ? PlayerRomfs.has(path) : find(path) >= 0; }

        byte[] read(String path) throws IOException {
            if (player) return PlayerRomfs.read(path);
            long i = find(path); if (i < 0) return null;
            long off = index.getLong((int) (24 + i * 24)), size = index.getLong((int) (24 + i * 24 + 8));
            if (size > Integer.MAX_VALUE - 16) throw new IOException(path + " is too big to merge");
            byte[] b = new byte[(int) size];
            synchronized (image) { image.seek(off); image.readFully(b); }
            return b;
        }

        /** SHA-256 of a RomFS file, streamed (originals can be large). */
        String sha(String path) throws IOException {
            if (player) return PlayerRomfs.sha(path);
            long i = find(path); if (i < 0) return null;
            long off = index.getLong((int) (24 + i * 24)), size = index.getLong((int) (24 + i * 24 + 8));
            MessageDigest d = digest(); byte[] buf = new byte[1 << 20];
            synchronized (image) {
                image.seek(off);
                for (long done = 0; done < size; ) { int n = (int) Math.min(buf.length, size - done); image.readFully(buf, 0, n); d.update(buf, 0, n); done += n; }
            }
            return hex(d.digest());
        }

        @Override public void close() throws IOException { indexFile.close(); image.close(); }
    }

    // ---- SARC ----

    static final class Member {
        final String name; final int hash; final byte[] data;
        Member(String name, int hash, byte[] data) { this.name = name; this.hash = hash; this.data = data; }
    }

    static final class Sarc {
        final List<Member> members = new ArrayList<>();
        byte[] version = {0, 1, 0, 0};

        static Sarc read(byte[] d) throws IOException {
            if (d.length < 20 || d[0] != 'S' || d[1] != 'A' || d[2] != 'R' || d[3] != 'C') throw new IOException("not a SARC");
            ByteBuffer b = le(d); int header = b.getShort(4) & 0xffff, start = b.getInt(12);
            Sarc s = new Sarc(); s.version = Arrays.copyOfRange(d, 16, 20);
            int count = b.getShort(header + 6) & 0xffff, names = header + 12 + count * 16 + 8;
            for (int i = 0; i < count; i++) {
                int e = header + 12 + i * 16, h = b.getInt(e), attr = b.getInt(e + 4), lo = b.getInt(e + 8), hi = b.getInt(e + 12);
                int at = names + (attr & 0xffffff) * 4, end = at; while (d[end] != 0) end++;
                s.members.add(new Member(new String(d, at, end - at, StandardCharsets.UTF_8), h, Arrays.copyOfRange(d, start + lo, start + hi)));
            }
            return s;
        }

        Map<String, byte[]> map() { Map<String, byte[]> m = new LinkedHashMap<>(); for (Member x : members) m.put(x.name, x.data); return m; }

        /** The game's own layout: data and every member 8-byte aligned, members in name-hash order. */
        byte[] write() {
            members.sort((a, b) -> Integer.compareUnsigned(a.hash, b.hash));
            ByteBuffer names = ByteBuffer.allocate(64 + members.stream().mapToInt(m -> m.name.length() * 3 + 8).sum()).order(ByteOrder.LITTLE_ENDIAN);
            int[] nameAt = new int[members.size()];
            for (int i = 0; i < members.size(); i++) {
                nameAt[i] = names.position() / 4; names.put(members.get(i).name.getBytes(StandardCharsets.UTF_8)); names.put((byte) 0);
                while (names.position() % 4 != 0) names.put((byte) 0);
            }
            int nameLen = names.position(), head = 0x14 + 0xc + 16 * members.size() + 8 + nameLen, start = (head + 7) & ~7;
            int[] lo = new int[members.size()], hi = new int[members.size()]; int data = 0;
            for (int i = 0; i < members.size(); i++) { data = (data + 7) & ~7; lo[i] = data; data += members.get(i).data.length; hi[i] = data; }
            ByteBuffer o = ByteBuffer.allocate(start + data).order(ByteOrder.LITTLE_ENDIAN);
            o.put(new byte[]{'S', 'A', 'R', 'C'}).putShort((short) 0x14).putShort((short) 0xfeff).putInt(start + data).putInt(start).put(version);
            o.put(new byte[]{'S', 'F', 'A', 'T'}).putShort((short) 0xc).putShort((short) members.size()).putInt(0x65);
            for (int i = 0; i < members.size(); i++) o.putInt(members.get(i).hash).putInt(0x01000000 | nameAt[i]).putInt(lo[i]).putInt(hi[i]);
            o.put(new byte[]{'S', 'F', 'N', 'T'}).putShort((short) 8).putShort((short) 0).put(names.array(), 0, nameLen);
            for (int i = 0; i < members.size(); i++) { o.position(start + lo[i]); o.put(members.get(i).data); }
            return o.array();
        }
    }

    /** The SFAT name hash (key 0x65, signed characters). */
    static int sarcHash(String name) {
        int h = 0; for (byte c : name.getBytes(StandardCharsets.UTF_8)) h = h * 0x65 + c; return h;
    }

    // ---- BYML v7, flattened to path -> value, only to compare documents field by field ----

    static Map<String, String> byml(byte[] b) {
        if (b == null || b.length < 16 || b[0] != 'Y' || b[1] != 'B') return null;
        ByteBuffer d = le(b);
        List<String> keys = bymlStrings(b, d, d.getInt(4)), strings = bymlStrings(b, d, d.getInt(8));
        Map<String, String> out = new HashMap<>();
        int root = d.getInt(12);
        if (root != 0) bymlNode(b, d, keys, strings, "", root, out);
        return out;
    }

    private static List<String> bymlStrings(byte[] b, ByteBuffer d, int off) {
        List<String> l = new ArrayList<>(); if (off == 0) return l;
        int n = d.getInt(off) >>> 8;
        for (int i = 0; i < n; i++) { int at = off + d.getInt(off + 4 + 4 * i), end = at; while (b[end] != 0) end++; l.add(new String(b, at, end - at, StandardCharsets.UTF_8)); }
        return l;
    }

    private static void bymlValue(byte[] b, ByteBuffer d, List<String> keys, List<String> strings, String path, int t, int v, Map<String, String> out) {
        if (t == 0xc0 || t == 0xc1) bymlNode(b, d, keys, strings, path, v, out);
        else if (t == 0xa0) out.put(path, "s:" + strings.get(v));
        else if (t >= 0xd4 && t <= 0xd6) out.put(path, t + ":" + d.getLong(v));
        else out.put(path, t + ":" + v);
    }

    private static void bymlNode(byte[] b, ByteBuffer d, List<String> keys, List<String> strings, String path, int o, Map<String, String> out) {
        int t = b[o] & 0xff, n = d.getInt(o) >>> 8;
        if (t == 0xc1) {
            for (int i = 0; i < n; i++) { int e = o + 4 + 8 * i; bymlValue(b, d, keys, strings, path + "." + keys.get(d.getInt(e) & 0xffffff), b[e + 3] & 0xff, d.getInt(e + 4), out); }
        } else if (t == 0xc0) {
            int base = o + 4 + ((n + 3) & ~3);
            for (int i = 0; i < n; i++) bymlValue(b, d, keys, strings, path + "[" + i + "]", b[o + 4 + i] & 0xff, d.getInt(base + 4 * i), out);
        } else out.put(path, "node:" + t);
    }

    // ---- the resource size table ----

    static final class Restbl {
        int version, slen;
        final HashMap<Integer, Integer> crc = new HashMap<>();
        final TreeMap<String, Integer> names = new TreeMap<>();

        static Restbl read(byte[] data) throws IOException {
            if (data.length < 22 || !new String(data, 0, 6, StandardCharsets.US_ASCII).equals("RESTBL")) throw new IOException("not a size table");
            ByteBuffer d = le(data); Restbl t = new Restbl();
            t.version = d.getInt(6); t.slen = d.getInt(10); int ncrc = d.getInt(14), nname = d.getInt(18);
            for (int i = 0; i < ncrc; i++) t.crc.put(d.getInt(22 + 8 * i), d.getInt(26 + 8 * i));
            int base = 22 + 8 * ncrc;
            for (int i = 0; i < nname; i++) {
                int e = base + i * (t.slen + 4), end = e; while (end < e + t.slen && data[end] != 0) end++;
                t.names.put(new String(data, e, end - e, StandardCharsets.UTF_8), d.getInt(e + t.slen));
            }
            return t;
        }

        static int crc32(String key) { CRC32 c = new CRC32(); c.update(key.getBytes(StandardCharsets.UTF_8)); return (int) c.getValue(); }

        /** The size of a RomFS path (".zs" / ".mc" dropped), 0 when the table does not list it. */
        long get(String path) {
            String k = key(path); Integer v = names.containsKey(k) ? names.get(k) : crc.get(crc32(k));
            return v == null ? 0 : v & 0xffffffffL;
        }

        void set(String path, long size) {
            String k = key(path); int v = (int) Math.min(size, 0xffffffffL);
            if (names.containsKey(k)) names.put(k, v); else crc.put(crc32(k), v);
        }

        static String key(String path) {
            for (String suffix : new String[]{".zs", ".mc"}) if (path.endsWith(suffix)) path = path.substring(0, path.length() - suffix.length());
            return path;
        }

        byte[] write() {
            ByteBuffer o = ByteBuffer.allocate(22 + 8 * crc.size() + (slen + 4) * names.size()).order(ByteOrder.LITTLE_ENDIAN);
            o.put("RESTBL".getBytes(StandardCharsets.US_ASCII)).putInt(version).putInt(slen).putInt(crc.size()).putInt(names.size());
            Integer[] ks = crc.keySet().toArray(new Integer[0]); Arrays.sort(ks, Integer::compareUnsigned);
            for (Integer k : ks) o.putInt(k).putInt(crc.get(k));
            for (Map.Entry<String, Integer> e : names.entrySet()) {
                byte[] n = e.getKey().getBytes(StandardCharsets.UTF_8); o.put(n); o.put(new byte[slen - n.length]); o.putInt(e.getValue());
            }
            return o.array();
        }
    }

    // ---- the 1.0.0 base (tools/make_mod_base.py) ----

    static final class BaseMember {
        final long hash; final byte[] byml;   /* byml: 1.0.0's bytes when it is BYML and 1.4.3's differs, else null */
        BaseMember(long hash, byte[] byml) { this.hash = hash; this.byml = byml; }
    }

    static final class Base implements Closeable {
        private final RandomAccessFile f;
        private final int files, packs; private final long filesOff, packsOff;

        Base(File file) throws IOException {
            f = new RandomAccessFile(file, "r");
            byte[] h = new byte[32]; f.readFully(h);
            if (!new String(h, 0, 8, StandardCharsets.US_ASCII).equals("TKMODB01")) throw new IOException("not a mod base file");
            ByteBuffer b = le(h); files = b.getInt(8); packs = b.getInt(12); filesOff = b.getLong(16); packsOff = b.getLong(24);
        }

        private long readKey(long at) throws IOException { f.seek(at); return f.readLong(); }   /* big endian, like key8 */

        private int search(long tableOff, int n, int stride, long key) throws IOException {
            int lo = 0, hi = n;
            while (lo < hi) { int m = (lo + hi) >>> 1; if (Long.compareUnsigned(readKey(tableOff + (long) m * stride), key) < 0) lo = m + 1; else hi = m; }
            return lo < n && readKey(tableOff + (long) lo * stride) == key ? lo : -1;
        }

        /** 1.0.0's hash of a file 1.4.3 changed; 0 when 1.4.3 has 1.0.0's file (or 1.0.0 lacks it). */
        synchronized long file(String path) throws IOException {
            int i = search(filesOff, files, 16, key8(path)); if (i < 0) return 0;
            f.seek(filesOff + (long) i * 16 + 8); return f.readLong();
        }

        /** 1.0.0's members of a pack 1.4.3 changed, or null (1.4.3's pack is 1.0.0's, or 1.0.0 lacks it). */
        synchronized Map<String, BaseMember> pack(String path) throws IOException {
            int i = search(packsOff, packs, 24, key8(path)); if (i < 0) return null;
            byte[] e = new byte[16]; f.seek(packsOff + (long) i * 24 + 8); f.readFully(e);
            ByteBuffer b = le(e); long off = b.getLong(0); int len = b.getInt(8);
            byte[] blob = new byte[len]; f.seek(off); f.readFully(blob);
            ByteBuffer d = le(Zstd.decompress(blob)); Map<String, BaseMember> m = new HashMap<>();
            int n = d.getInt();
            for (int k = 0; k < n; k++) {
                byte[] name = new byte[d.getShort() & 0xffff]; d.get(name);
                long hash = d.order(ByteOrder.BIG_ENDIAN).getLong(); d.order(ByteOrder.LITTLE_ENDIAN);
                int size = d.getInt(); byte[] body = null; if (size > 0) { body = new byte[size]; d.get(body); }
                m.put(new String(name, StandardCharsets.UTF_8), new BaseMember(hash, body));
            }
            return m;
        }

        @Override public void close() throws IOException { f.close(); }
    }
}

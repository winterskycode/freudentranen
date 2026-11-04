package dev.tok.totk;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** The startup screen's mods (the Mods page). Each installed mod is a folder in mods/: its RomFS files (romfs/), what the
    installer found (mod.json: name, author, contents, the versions it was made for, warnings) and one line per file
    (files.tsv). mods/order.txt holds the load order, top first (the top wins where mods overlap), and which are on.
    Before a start with mods on, apply() builds mods/active: one manifest for runtime/romfs.c whose lines point into the
    mods' folders, plus the files no single mod has: packs merged member by member (three ways, 1.0.0 as the base: what
    a mod changed is kept, and so is what 1.4.3 changed after the mod was made) and the 1.4.3 size table with room for
    the mods' files. After a start, lastRun() reads the log for a failed start or a crash and names the mod involved. */
final class Mods {
    static final String DATA = Game.DATA;
    static File ROOT = new File(DATA, "mods"), ACTIVE = new File(ROOT, "active"), ORDER = new File(ROOT, "order.txt");
    static final File INBOX = new File("/sdcard/tok/mods-inbox");
    static File BASE = new File(DATA, "mod-base.bin");
    private static android.content.Context context;
    /** A player's app has no all-files access: the mods and the mod base, made once, live in the app's storage. */
    static void context(android.content.Context c) {
        context = c;
        if (android.os.Environment.isExternalStorageManager()) { if (BASE.exists()) return; }
        else { ROOT = new File(c.getFilesDir(), "mods"); ACTIVE = new File(ROOT, "active"); ORDER = new File(ROOT, "order.txt"); }
        BASE = new File(c.getFilesDir(), "mod-base.bin");
    }
    static final String RSTB = "System/Resource/ResourceSizeTable.Product.143.Nin_NX_NVN.rsizetable.zs";
    /** Built-in mods (ids starting with "_"): not listed on the Mods page, and merged when a setting asks for them, below the
        player's mods. The PlayStation button icons: made here from the player's files (IconMod); for prepared data under
        /sdcard/tok, pushed with it (tools/make_icon_mod.py). */
    static final String ICONS_PLAYSTATION = "_buttons-playstation";

    interface Progress { void step(String what); }

    static final class Mod {
        String id, name = "", author = "", version = "", description = "", source = "", made = "";
        long installed, bytes; int replaced, added, skipped;
        boolean on = true;
        final List<String> contents = new ArrayList<>(), warnings = new ArrayList<>(), notes = new ArrayList<>();
        String problem;   /* from the last start: a failed start or a crash involving this mod */
        File dir() { return new File(ROOT, id); }
        File preview() { for (String n : new String[]{"preview.png", "preview.jpg"}) { File f = new File(dir(), n); if (f.exists()) return f; } return null; }
    }

    /** One line of a mod's files.tsv. kind: copy, pack, same (equal to 1.4.3's), stale (1.0.0's copy of a file 1.4.3
        changed), table (a size table: read for sizes, not installed). */
    static final class Entry {
        String path, sha, orig, kind; long size, hint;
        String line() { return path + "\t" + size + "\t" + sha + "\t" + orig + "\t" + kind + "\t" + hint; }
        static Entry parse(String l) {
            String[] f = l.split("\t"); if (f.length < 6) return null;
            Entry e = new Entry(); e.path = f[0]; e.size = Long.parseLong(f[1]); e.sha = f[2]; e.orig = f[3]; e.kind = f[4]; e.hint = Long.parseLong(f[5]); return e;
        }
    }

    // ---- the list ----

    static List<Mod> list() {
        List<Mod> out = new ArrayList<>(); Set<String> seen = new LinkedHashSet<>();
        for (String line : lines(ORDER)) {
            String[] f = line.split("\t"); if (f.length != 2 || seen.contains(f[1]) || f[1].startsWith("_")) continue;
            Mod m = load(f[1]); if (m == null) continue;
            m.on = f[0].equals("on"); out.add(m); seen.add(m.id);
        }
        String[] dirs = ROOT.list();   // installed but not in the list (an interrupted save): at the bottom, off
        if (dirs != null) { Arrays.sort(dirs); for (String d : dirs) if (!seen.contains(d) && !d.startsWith("_") && new File(ROOT, d + "/mod.json").exists()) { Mod m = load(d); if (m != null) { m.on = false; out.add(m); } } }
        Map<String, List<String>> notes = new HashMap<>();
        for (String l : lines(new File(ACTIVE, "notes.tsv"))) { int t = l.indexOf('\t'); if (t > 0) notes.computeIfAbsent(l.substring(0, t), k -> new ArrayList<>()).add(l.substring(t + 1)); }
        List<Mod> withIcons = enabled(out); Mod icons = builtIn(ICONS_PLAYSTATION); if (icons != null) withIcons.add(icons);
        if (upToDate(enabled(out)) || upToDate(withIcons)) for (Mod m : out) if (notes.containsKey(m.id)) m.notes.addAll(notes.get(m.id));   // only while they describe this list
        return out;
    }

    /** A built-in mod, on, or null when it is not on the device. */
    static Mod builtIn(String id) { Mod m = new File(ROOT, id + "/mod.json").exists() ? load(id) : null; if (m != null) m.on = true; return m; }

    static void save(List<Mod> mods) {
        StringBuilder s = new StringBuilder();
        for (Mod m : mods) s.append(m.on ? "on" : "off").append('\t').append(m.id).append('\n');
        write(ORDER, s.toString());
    }

    static void remove(List<Mod> mods, Mod m) { mods.remove(m); save(mods); deleteTree(m.dir()); }

    private static Mod load(String id) {
        try {
            JSONObject j = new JSONObject(new String(Files.readAllBytes(new File(ROOT, id + "/mod.json").toPath()), StandardCharsets.UTF_8));
            Mod m = new Mod(); m.id = id;
            m.name = j.optString("name"); m.author = j.optString("author"); m.version = j.optString("version"); m.description = j.optString("description");
            m.source = j.optString("source"); m.made = j.optString("made"); m.installed = j.optLong("installed"); m.bytes = j.optLong("bytes");
            m.replaced = j.optInt("replaced"); m.added = j.optInt("added"); m.skipped = j.optInt("skipped");
            strings(j.optJSONArray("contents"), m.contents); strings(j.optJSONArray("warnings"), m.warnings);
            return m;
        } catch (IOException | JSONException e) { return null; }
    }

    private static void strings(JSONArray a, List<String> out) { if (a != null) for (int i = 0; i < a.length(); i++) out.add(a.optString(i)); }

    // ---- what can be installed ----

    /** Archives and mod folders in Download and the mods inbox, newest first. */
    static List<File> candidates() {
        List<File> out = new ArrayList<>();
        INBOX.mkdirs();
        for (File dir : new File[]{new File("/sdcard/Download"), INBOX}) {
            File[] fs = dir.listFiles(); if (fs == null) continue;
            for (File f : fs) {
                String n = f.getName().toLowerCase(Locale.ROOT);
                if (f.isFile() && n.endsWith(".zip") && zipHasRomfs(f)) out.add(f);
                else if (f.isDirectory() && findRomfs(f, 4).size() > 0) out.add(f);
            }
        }
        out.sort((a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        return out;
    }

    /** A zip whose entries include a RomFS folder (romfs/, or the RomFS's own top folders such as Model/ or Pack/). */
    private static boolean zipHasRomfs(File f) {
        try (ZipFile z = new ZipFile(f)) {
            for (ZipEntry e : Collections.list(z.entries())) {
                String[] parts = e.getName().toLowerCase(Locale.ROOT).split("/");
                for (String part : parts) if (part.equals("romfs")) return true;
                for (int i = 0; i + 1 < parts.length && i < 3; i++) if (ROMFS_DIRS.contains(parts[i]) && !parts[i].equals("system")) return true;
            }
        } catch (IOException ignored) {}
        return false;
    }

    /** A log line of the game refusing a mod's file at start (runtime/romfs.c overlay checks) */
    private static boolean overlayFailure(String l) {
        if (l.startsWith("[totk] FAIL:")) return l.contains("overlay");
        if (!l.startsWith("[totk] RomFS overlay:")) return false;
        for (String why : new String[] {"mismatch", "missing", "lacks", "already has", "the same", "changed"}) if (l.contains(why)) return true;
        return false;
    }

    // ---- install ----

    private static ModData.Romfs romfs;
    private static ModData.Base base;

    /** The RomFS image, the zstd dictionaries and (when present) the 1.0.0 base, opened once. */
    private static synchronized ModData.Romfs env() throws IOException {
        if (romfs == null) {
            romfs = context != null && Game.SETUP && SetupFiles.openForMods(context) ? new ModData.Romfs() : new ModData.Romfs(new File(DATA, "romfs"));
            List<byte[]> dicts = new ArrayList<>();
            for (ModData.Member m : ModData.Sarc.read(Zstd.decompress(romfs.read("Pack/ZsDic.pack.zs"))).members) dicts.add(m.data);
            Zstd.dictionaries(dicts.toArray(new byte[0][]));
        }
        if (romfs.player && !BASE.exists()) PlayerRomfs.makeModBase(BASE.getPath());
        if (base == null && BASE.exists()) base = new ModData.Base(BASE);
        return romfs;
    }

    static Mod install(File source, Progress p) throws IOException {
        ROOT.mkdirs();
        ModData.Romfs rom = env();
        File stage = new File(ROOT, ".incoming-" + System.currentTimeMillis());
        try {
            File top; stage.mkdirs();
            if (source.isDirectory()) top = source;
            else { p.step("Unpacking " + source.getName()); unzip(source, stage, p); top = stage; }
            List<File> roots = findRomfs(top, 6);
            if (roots.isEmpty()) throw new IOException(source.getName() + " contains no RomFS files. A mod should have a romfs folder, or folders such as Model, Pack and TexToGo.");
            File root = pick(roots, top);
            Mod m = new Mod();
            m.source = source.getName(); m.installed = System.currentTimeMillis();
            if (roots.size() > 1) m.warnings.add(roots.size() + " versions of the mod are in the archive; installed " + rel(top, root) + ".");
            if (exists(top, "exefs", 6)) m.warnings.add("This mod includes code patches (exefs). This port can only use RomFS files, so the code part is not installed.");
            if (exists(top, "cheats", 6)) m.warnings.add("This mod includes emulator cheats, which this port cannot use.");
            metadata(m, top, root.getName().equalsIgnoreCase("romfs") ? root.getParentFile() : root, stage);
            if (m.name.isEmpty()) m.name = clean(root.getName().equalsIgnoreCase("romfs") && !root.getParentFile().equals(top) ? root.getParentFile().getName() : strip(source.getName()));
            m.id = slug(m.name) + "-" + Long.toHexString(m.installed & 0xffffff);
            File dir = m.dir(); dir.mkdirs();
            File dest = new File(dir, "romfs"); dest.mkdirs();
            File[] parts = root.listFiles();   // the RomFS folders; loose files beside them (a readme, a picture) are not game files
            if (parts != null) for (File f : parts) {
                if (!f.isDirectory() || f.getName().startsWith(".")) continue;
                if (source.isDirectory() || !f.renameTo(new File(dest, f.getName()))) { p.step("Copying " + f.getName()); copyTree(f, new File(dest, f.getName())); }
            }
            File preview = new File(stage, ".preview");
            if (preview.exists()) { String ext = new String(Files.readAllBytes(new File(stage, ".preview-ext").toPath())); preview.renameTo(new File(dir, "preview." + ext)); }
            analyse(m, rom, dest, p);
            writeMod(m);
            List<Mod> mods = list(); mods.removeIf(x -> x.id.equals(m.id)); mods.add(0, m); save(mods);
            return m;
        } finally { deleteTree(stage); }
    }

    private static void unzip(File zip, File into, Progress p) throws IOException {
        String canon = into.getCanonicalPath() + File.separator;
        try (ZipFile z = new ZipFile(zip)) {
            int n = 0, total = z.size();
            for (ZipEntry e : Collections.list(z.entries())) {
                File out = new File(into, e.getName());
                if (!out.getCanonicalPath().startsWith(canon)) throw new IOException("The archive contains an unsafe file path: " + e.getName());
                if (e.isDirectory()) { out.mkdirs(); continue; }
                out.getParentFile().mkdirs();
                try (InputStream in = z.getInputStream(e); OutputStream o = new FileOutputStream(out)) { copy(in, o); }
                if (++n % 50 == 0) p.step(String.format(Locale.ROOT, "Unpacking %d / %d", n, total));
            }
        }
    }

    private static final Set<String> ROMFS_DIRS = new LinkedHashSet<>(Arrays.asList("actor", "ai", "banc", "effect", "event", "font", "gamedata",
        "layout", "mals", "model", "movie", "pack", "phive", "sequence", "shader", "sound", "system", "textogo", "ui", "vfr", "rsdb", "logic", "nmesh"));

    /** Folders that are a RomFS root: named romfs, or holding the RomFS's own top folders. */
    static List<File> findRomfs(File dir, int depth) {
        List<File> out = new ArrayList<>(); File[] fs = dir.listFiles(); if (fs == null) return out;
        int known = 0;
        for (File f : fs) if (f.isDirectory() && ROMFS_DIRS.contains(f.getName().toLowerCase(Locale.ROOT))) known++;
        if (known >= 1 && !dir.getName().equalsIgnoreCase("exefs")) { out.add(dir); return out; }
        if (depth == 0) return out;
        Arrays.sort(fs);
        for (File f : fs) {
            if (!f.isDirectory()) continue;
            if (f.getName().equalsIgnoreCase("romfs")) out.add(f); else out.addAll(findRomfs(f, depth - 1));
        }
        return out;
    }

    /** Of several RomFS folders, one whose path names 1.4 (the game's version), else the first. */
    private static File pick(List<File> roots, File top) {
        for (File r : roots) { String s = rel(top, r).toLowerCase(Locale.ROOT); if (s.contains("1.4") || s.contains("143") || s.contains("1.4.x")) return r; }
        return roots.get(0);
    }

    private static boolean exists(File dir, String name, int depth) {
        File[] fs = dir.listFiles(); if (fs == null) return false;
        for (File f : fs) if (f.isDirectory() && (f.getName().equalsIgnoreCase(name) || (depth > 0 && exists(f, name, depth - 1)))) return true;
        return false;
    }

    /** Name, author, version and description from info.json or rules.txt (Cemu's [Definition]) next to the RomFS or at
        the archive's top, and a preview picture. Most mods have none: the name then comes from the folder or archive. */
    private static void metadata(Mod m, File top, File modDir, File stage) {
        for (File dir : new File[]{modDir, top}) {
            File info = new File(dir, "info.json");
            if (m.name.isEmpty() && info.exists()) {
                try {
                    JSONObject j = new JSONObject(new String(Files.readAllBytes(info.toPath()), StandardCharsets.UTF_8));
                    m.name = first(j, "name", "title"); m.author = first(j, "author", "authors", "creator", "creators");
                    m.version = first(j, "version"); m.description = first(j, "description", "desc");
                } catch (IOException | JSONException ignored) {}
            }
            File rules = new File(dir, "rules.txt");
            if (m.name.isEmpty() && rules.exists()) {
                for (String l : lines(rules)) {
                    int eq = l.indexOf('='); if (eq < 0) continue;
                    String k = l.substring(0, eq).trim().toLowerCase(Locale.ROOT), v = l.substring(eq + 1).trim().replace("|", "\n");
                    if (k.equals("name")) m.name = v; else if (k.equals("description")) m.description = v; else if (k.equals("version")) m.version = v;
                }
            }
            if (!new File(stage, ".preview").exists()) {
                File[] fs = dir.listFiles(); File any = null; int images = 0;
                if (fs != null) for (File f : fs) {
                    String n = f.getName().toLowerCase(Locale.ROOT);
                    if (!f.isFile() || !(n.endsWith(".png") || n.endsWith(".jpg") || n.endsWith(".jpeg"))) continue;
                    images++; if (any == null || n.startsWith("preview") || n.startsWith("thumbnail") || n.startsWith("icon") || n.startsWith("banner") || n.startsWith("cover")) any = f;
                }
                if (any != null && (images == 1 || any.getName().toLowerCase(Locale.ROOT).matches("(preview|thumbnail|icon|banner|cover).*"))) {
                    try {
                        Files.copy(any.toPath(), new File(stage, ".preview").toPath());
                        write(new File(stage, ".preview-ext"), any.getName().toLowerCase(Locale.ROOT).endsWith(".png") ? "png" : "jpg");
                    } catch (IOException ignored) {}
                }
            }
        }
    }

    private static String first(JSONObject j, String... keys) {
        for (String k : keys) {
            Object v = j.opt(k);
            if (v instanceof JSONArray) { List<String> l = new ArrayList<>(); strings((JSONArray) v, l); if (!l.isEmpty()) return String.join(", ", l); }
            else if (v != null && !String.valueOf(v).isEmpty()) return String.valueOf(v);
        }
        return "";
    }

    private static final Pattern PRODUCT = Pattern.compile("ResourceSizeTable\\.Product\\.(\\d)(\\d)(\\d)");

    /** Each file against 1.4.3 (and 1.0.0): files.tsv, the counts, what the mod changes and which versions its size
        tables are for. */
    private static void analyse(Mod m, ModData.Romfs rom, File dir, Progress p) throws IOException {
        List<File> files = new ArrayList<>(); walk(dir, files);
        List<Entry> entries = new ArrayList<>(); List<ModData.Restbl> tables = new ArrayList<>(); List<String> made = new ArrayList<>();
        Map<String, Integer> kinds = new TreeMap<>();
        int n = 0;
        for (File f : files) {
            if (++n % 20 == 0) p.step(String.format(Locale.ROOT, "Checking files %d / %d", n, files.size()));
            String path = rel(dir, f).replace(File.separatorChar, '/');
            Entry e = new Entry(); e.path = path; e.size = f.length(); e.hint = 0;
            byte[] sha = shaFile(f); e.sha = ModData.hex(sha);
            Matcher pm = PRODUCT.matcher(path);
            if (path.startsWith("System/Resource/ResourceSizeTable.")) {
                e.kind = "table"; e.orig = "-"; entries.add(e);
                if (pm.find()) made.add(pm.group(1) + "." + pm.group(2) + "." + pm.group(3));
                try { tables.add(ModData.Restbl.read(unzs(Files.readAllBytes(f.toPath())))); } catch (IOException ex) { m.warnings.add("Its size table " + f.getName() + " could not be read."); }
                continue;
            }
            String orig = rom.sha(path);
            e.orig = orig == null ? "-" : orig;
            long v0 = base != null && orig != null ? base.file(path) : 0;
            long mine = java.nio.ByteBuffer.wrap(sha).getLong();
            if (orig != null && orig.equals(e.sha)) e.kind = "same";
            else if (v0 != 0 && v0 == mine) e.kind = "stale";
            else if (orig != null && path.endsWith(".pack.zs")) e.kind = "pack";
            else e.kind = "copy";
            if (e.kind.equals("copy") || e.kind.equals("pack")) { if (orig == null) m.added++; else m.replaced++; m.bytes += e.size; kinds.merge(category(path), 1, Integer::sum); }
            else m.skipped++;
            entries.add(e);
        }
        for (Entry e : entries) for (ModData.Restbl t : tables) e.hint = Math.max(e.hint, t.get(e.path));
        for (Map.Entry<String, Integer> k : kinds.entrySet()) m.contents.add(k.getKey() + " (" + k.getValue() + ")");
        Collections.sort(made);
        if (!made.isEmpty()) {
            m.made = made.size() == 1 ? made.get(0) : made.get(0) + " – " + made.get(made.size() - 1);
            if (!made.contains("1.4.3")) m.warnings.add("Its size tables show that it was made for game version " + m.made + ". Its files are merged into 1.4.3, but files that 1.4.3 has changed since then may not work.");
        }
        if (base == null && entries.stream().anyMatch(x -> x.kind.equals("pack"))) m.warnings.add("The 1.0.0 base (mod-base.bin) is missing: its packs replace 1.4.3's whole instead of being merged.");
        if (m.skipped > 0) m.warnings.add(m.skipped + (m.skipped == 1 ? " file is" : " files are") + " left out: equal to 1.4.3's, or an old copy of a file 1.4.3 changed.");
        if (m.replaced + m.added == 0) m.warnings.add("This mod changes nothing in 1.4.3.");
        StringBuilder s = new StringBuilder(); for (Entry e : entries) s.append(e.line()).append('\n');
        write(new File(m.dir(), "files.tsv"), s.toString());
    }

    private static String category(String path) {
        String top = path.contains("/") ? path.substring(0, path.indexOf('/')) : path;
        switch (top) {
            case "Model": return "models";
            case "TexToGo": return "textures";
            case "Pack": return path.startsWith("Pack/Actor/") ? "actors" : "packs";
            case "UI": return "interface";
            case "Mals": return "text";
            case "Sound": return "sound";
            case "Event": return "events";
            case "Banc": return "world placement";
            case "Phive": return "physics";
            case "Effect": return "effects";
            case "Shader": return "shaders";
            case "AI": case "Logic": case "Sequence": return "behaviour";
            case "Movie": return "movies";
            case "GameData": case "RSDB": return "game data";
            default: return top;
        }
    }

    static void writeMod(Mod m) throws IOException {
        try {
            JSONObject j = new JSONObject();
            j.put("name", m.name).put("author", m.author).put("version", m.version).put("description", m.description).put("source", m.source).put("made", m.made)
             .put("installed", m.installed).put("bytes", m.bytes).put("replaced", m.replaced).put("added", m.added).put("skipped", m.skipped)
             .put("contents", new JSONArray(m.contents)).put("warnings", new JSONArray(m.warnings));
            write(new File(m.dir(), "mod.json"), j.toString(2));
        } catch (JSONException e) { throw new IOException(e); }
    }

    // ---- the built-in icons ----

    /** Makes the PlayStation button icons from the game files. Null when they are ready, else why not, in plain words. */
    static String makeIcons(byte[] art, Progress p) {
        String why;
        try { ModData.Romfs rom = env(); ROOT.mkdirs(); IconMod.make(rom::read, art, p); return null; }
        catch (IOException e) { why = e.getMessage(); }
        catch (RuntimeException e) { why = "The game files are not as expected."; }
        catch (OutOfMemoryError e) { why = "The app ran out of memory."; }
        return "The PlayStation buttons could not be made, so the game shows Switch buttons. " + why;
    }

    // ---- the merged set the game loads ----

    private static String signature(List<Mod> on) {
        StringBuilder s = new StringBuilder("v1 base=" + BASE.length() + "\n");
        for (Mod m : on) s.append(m.id).append(' ').append(new File(m.dir(), "files.tsv").lastModified()).append('\n');
        return s.toString();
    }

    static List<Mod> enabled(List<Mod> mods) { List<Mod> on = new ArrayList<>(); for (Mod m : mods) if (m.on) on.add(m); return on; }

    /** Whether mods/active was built for these mods (the ones that are on, in order, built-ins included). */
    static boolean upToDate(List<Mod> on) {
        File sig = new File(ACTIVE, "signature"), man = new File(ACTIVE, "manifest.tsv");
        return sig.exists() && man.exists() && read(sig).equals(signature(on));
    }

    /** Builds mods/active for these mods (the ones that are on, built-ins included), top of the list winning. */
    static void apply(List<Mod> on, Progress p) throws IOException {
        ModData.Romfs rom = env();
        deleteTree(ACTIVE); File gen = new File(ACTIVE, "gen"); gen.mkdirs();
        Map<String, List<Object[]>> by = new LinkedHashMap<>();   // path -> {mod, entry}, top first
        for (Mod m : on) for (String l : lines(new File(m.dir(), "files.tsv"))) {
            Entry e = Entry.parse(l); if (e == null || !(e.kind.equals("copy") || e.kind.equals("pack"))) continue;
            by.computeIfAbsent(e.path, k -> new ArrayList<>()).add(new Object[]{m, e});
        }
        Map<String, List<String>> notes = new LinkedHashMap<>(); Map<String, Set<String>> sources = new TreeMap<>();
        StringBuilder manifest = new StringBuilder("# the mods that are on, merged by the startup screen (Mods.java); top of the list wins\n");
        ModData.Restbl table = ModData.Restbl.read(unzs(rom.read(RSTB))); boolean tableChanged = false;
        int done = 0;
        for (Map.Entry<String, List<Object[]>> it : by.entrySet()) {
            String path = it.getKey(); List<Object[]> from = it.getValue();
            if (++done % 25 == 0) p.step(String.format(Locale.ROOT, "Merging mods %d / %d", done, by.size()));
            Mod top = (Mod) from.get(0)[0]; Entry te = (Entry) from.get(0)[1];
            long hint = 0; for (Object[] f : from) hint = Math.max(hint, ((Entry) f[1]).hint);
            if (te.kind.equals("pack")) {
                byte[] orig = rom.read(path); byte[] packed = mergePack(path, orig, from, notes);
                Set<String> ids = new LinkedHashSet<>(); for (Object[] f : from) ids.add(((Mod) f[0]).id);
                if (packed == null) continue;   /* the mods' changes to it were all 1.4.3's already */
                File out = new File(gen, path); out.getParentFile().mkdirs();
                byte[] z = Zstd.compress(packed, Zstd.dictionaryOf(orig)); writeBytes(out, z);
                long grown = table.get(path) + Math.max(0, packed.length - unzs(orig).length);
                long size = Math.max(Math.max(table.get(path), hint), (grown + 31) & ~31L);
                if (size != table.get(path)) { table.set(path, size); tableChanged = true; }
                manifest.append(path).append('\t').append(z.length).append('\t').append(ModData.hex(ModData.sha256(z))).append('\t').append(te.orig).append('\t').append("gen/").append(path).append('\n');
                sources.put(path, ids);
            } else {
                for (int k = 1; k < from.size(); k++) note(notes, ((Mod) from.get(k)[0]).id, "Overridden by " + top.name + ": " + path);
                manifest.append(path).append('\t').append(te.size).append('\t').append(te.sha).append('\t').append(te.orig).append('\t')
                        .append(new File(top.dir(), "romfs/" + path).getPath()).append('\n');
                long size = Math.max(table.get(path), hint);
                if (size != table.get(path)) { table.set(path, size); tableChanged = true; }
                sources.put(path, new LinkedHashSet<>(Collections.singletonList(top.id)));
            }
        }
        if (tableChanged) {
            p.step("Writing the size table");
            byte[] z = Zstd.compress(table.write(), 0); File out = new File(gen, RSTB); out.getParentFile().mkdirs(); writeBytes(out, z);
            manifest.append(RSTB).append('\t').append(z.length).append('\t').append(ModData.hex(ModData.sha256(z))).append('\t').append(rom.sha(RSTB)).append('\t').append("gen/").append(RSTB).append('\n');
        }
        write(new File(ACTIVE, "manifest.tsv"), manifest.toString());
        StringBuilder src = new StringBuilder(); for (Map.Entry<String, Set<String>> e : sources.entrySet()) src.append(e.getKey()).append('\t').append(String.join(",", e.getValue())).append('\n');
        write(new File(ACTIVE, "sources.tsv"), src.toString());
        StringBuilder ns = new StringBuilder(); for (Map.Entry<String, List<String>> e : notes.entrySet()) for (String n : e.getValue()) ns.append(e.getKey()).append('\t').append(n).append('\n');
        write(new File(ACTIVE, "notes.tsv"), ns.toString());
        write(new File(ACTIVE, "signature"), signature(on));
    }

    private static void note(Map<String, List<String>> notes, String id, String s) { notes.computeIfAbsent(id, k -> new ArrayList<>()).add(s); }

    /** One pack: 1.4.3's members, each mod applied from the bottom of the list up. A member a mod left at 1.0.0's (or
        at 1.4.3's) stays; one only the mod changed takes the mod's; where 1.4.3 changed it too, a BYML member is merged
        field by field and the mod's is used when that keeps 1.4.3's changes, else 1.4.3's stays. Between two mods, the
        higher one wins. A member the mod adds is added; one 1.4.3 removed stays out. null: nothing to change. */
    private static byte[] mergePack(String path, byte[] origZs, List<Object[]> from, Map<String, List<String>> notes) throws IOException {
        ModData.Sarc orig = ModData.Sarc.read(unzs(origZs));
        Map<String, byte[]> origMap = orig.map(), cur = new LinkedHashMap<>(origMap);
        Map<String, Integer> hashes = new HashMap<>(); for (ModData.Member x : orig.members) hashes.put(x.name, x.hash);
        Map<String, ModData.BaseMember> baseMap = base != null ? base.pack(path) : null;
        Map<String, String> changedBy = new HashMap<>();
        for (int k = from.size() - 1; k >= 0; k--) {
            Mod m = (Mod) from.get(k)[0];
            Map<String, byte[]> mine = ModData.Sarc.read(unzs(Files.readAllBytes(new File(m.dir(), "romfs/" + path).toPath()))).map();
            for (Map.Entry<String, byte[]> it : mine.entrySet()) {
                String name = it.getKey(); byte[] b = it.getValue(), c = cur.get(name), o = origMap.get(name);
                ModData.BaseMember bm = baseMap != null ? baseMap.get(name) : o != null ? new ModData.BaseMember(ModData.hash8(o), null) : null;
                if (c == null) {
                    if (bm == null) { cur.put(name, b); changedBy.put(name, m.id); }
                    else note(notes, m.id, "Left out " + name + " from " + path.substring(path.lastIndexOf('/') + 1) + ", because 1.4.3 removed it.");
                    continue;
                }
                if (Arrays.equals(b, c)) continue;
                long hb = ModData.hash8(b);
                if (bm != null && hb == bm.hash) continue;                                      // the mod has 1.0.0's
                if (bm != null && ModData.hash8(c) == bm.hash) { cur.put(name, b); changedBy.put(name, m.id); continue; }   // only the mod changed it
                if (changedBy.containsKey(name)) {                                               // two mods changed it: the higher one wins
                    note(notes, changedBy.get(name), "Overridden by " + m.name + ": " + name + " in " + path);
                    cur.put(name, b); changedBy.put(name, m.id); continue;
                }
                byte[] baseBytes = bm == null ? null : bm.byml != null ? bm.byml : (o != null && ModData.hash8(o) == bm.hash ? o : null);
                Map<String, String> fb = ModData.byml(baseBytes), fc = ModData.byml(c), fm = ModData.byml(b);
                if (fb == null || fc == null || fm == null) { note(notes, m.id, "Kept 1.4.3's version of " + name + ". Both this mod and 1.4.3 changed it, and it is not a file type that can be merged."); continue; }
                Map<String, String> merged = new HashMap<>();
                Set<String> keys = new LinkedHashSet<>(fb.keySet()); keys.addAll(fc.keySet()); keys.addAll(fm.keySet());
                for (String key : keys) {
                    String vb = fb.get(key), vc = fc.get(key), vm = fm.get(key);
                    String v = eq(vm, vb) ? vc : eq(vc, vb) ? vm : vc;
                    if (v != null) merged.put(key, v);
                }
                if (merged.equals(fm)) { cur.put(name, b); changedBy.put(name, m.id); }
                else if (!merged.equals(fc)) note(notes, m.id, "Kept 1.4.3's version of " + name + ", because both changed the same values.");
            }
        }
        boolean same = cur.size() == origMap.size();
        if (same) for (Map.Entry<String, byte[]> e : cur.entrySet()) if (!Arrays.equals(e.getValue(), origMap.get(e.getKey()))) { same = false; break; }
        if (same) return null;
        ModData.Sarc out = new ModData.Sarc(); out.version = orig.version;
        for (Map.Entry<String, byte[]> e : cur.entrySet()) { Integer h = hashes.get(e.getKey()); out.members.add(new ModData.Member(e.getKey(), h != null ? h : ModData.sarcHash(e.getKey()), e.getValue())); }
        return out.write();
    }

    private static boolean eq(String a, String b) { return a == null ? b == null : a.equals(b); }

    // ---- the last start ----

    /** A failed start or a crash in the last run's log, set on the mods involved; the text for the Mods page when none
        can be named (null: nothing wrong, or mods were off). */
    static String lastRun(File log, List<Mod> mods) {
        String head = slice(log, false), tail = slice(log, true);
        if (!head.contains(ACTIVE.getPath()) && !tail.contains(ACTIVE.getPath())) return null;
        Map<String, Mod> byId = new HashMap<>(); for (Mod m : mods) byId.put(m.id, m);
        Map<String, String> sources = new HashMap<>();
        for (String l : lines(new File(ACTIVE, "sources.tsv"))) { int t = l.indexOf('\t'); if (t > 0) sources.put(l.substring(0, t), l.substring(t + 1)); }
        String text = head + "\n" + tail; String[] lines = text.split("\n");
        for (String l : lines) if (overlayFailure(l)) {
            List<Mod> who = involved(l, byId, sources);
            String msg = "The game stopped at start: " + l.replaceFirst("^\\[totk\\] (FAIL: |RomFS overlay: )", "");
            if (who.isEmpty()) return msg;
            for (Mod m : who) m.problem = msg;
            return null;
        }
        int crash = -1; for (int i = 0; i < lines.length; i++) if (lines[i].startsWith("[totk] signal=0x")) { crash = i; break; }
        if (crash < 0) return null;
        for (int i = crash - 1; i >= 0; i--) if (lines[i].startsWith("[totk] RomFS overlay open: ")) {
            String l = lines[i]; String name = l.substring("[totk] RomFS overlay open: ".length()); int from = name.indexOf(" from "); if (from > 0) name = name.substring(0, from);
            List<Mod> who = involved(l, byId, sources);
            for (Mod m : who) m.problem = "The game crashed at the last start. The last modded file it read was " + name + (who.size() > 1 ? ", merged from several mods." : ", from this mod.");
            if (!who.isEmpty()) return null;
            break;
        }
        return "The game crashed at the last start with mods on, before it read any modded file.";
    }

    private static List<Mod> involved(String line, Map<String, Mod> byId, Map<String, String> sources) {
        List<Mod> out = new ArrayList<>();
        Matcher m = Pattern.compile(Pattern.quote(ROOT.getPath()) + "/([^/]+)/romfs/").matcher(line);
        if (m.find() && byId.containsKey(m.group(1))) { out.add(byId.get(m.group(1))); return out; }
        Matcher g = Pattern.compile(Pattern.quote(ACTIVE.getPath()) + "/gen/(\\S+)").matcher(line);
        if (g.find() && sources.containsKey(g.group(1))) for (String id : sources.get(g.group(1)).split(",")) if (byId.containsKey(id)) out.add(byId.get(id));
        return out;
    }

    /** The first or last 512 KB of the log. */
    private static String slice(File f, boolean end) {
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            long n = r.length(), from = end ? Math.max(0, n - (512 << 10)) : 0; int len = (int) Math.min(512 << 10, n - from);
            byte[] b = new byte[len]; r.seek(from); r.readFully(b); return new String(b, StandardCharsets.UTF_8);
        } catch (IOException e) { return ""; }
    }

    // ---- files ----

    static byte[] unzs(byte[] b) throws IOException {
        return b.length >= 4 && (b[0] & 0xff) == 0x28 && (b[1] & 0xff) == 0xb5 && (b[2] & 0xff) == 0x2f && (b[3] & 0xff) == 0xfd ? Zstd.decompress(b) : b;
    }

    private static byte[] shaFile(File f) throws IOException {
        MessageDigest d = ModData.digest(); byte[] buf = new byte[1 << 20];
        try (InputStream in = new FileInputStream(f)) { for (int n; (n = in.read(buf)) > 0; ) d.update(buf, 0, n); }
        return d.digest();
    }

    private static void walk(File dir, List<File> out) {
        File[] fs = dir.listFiles(); if (fs == null) return; Arrays.sort(fs);
        for (File f : fs) if (f.isDirectory()) walk(f, out); else if (!f.getName().startsWith(".")) out.add(f);
    }

    static String rel(File base, File f) { String b = base.getPath(), p = f.getPath(); return p.startsWith(b + "/") ? p.substring(b.length() + 1) : p.equals(b) ? "" : p; }

    private static String strip(String name) { int dot = name.lastIndexOf('.'); return dot > 0 ? name.substring(0, dot) : name; }
    private static String clean(String s) { return s.replace('_', ' ').replaceAll("\\s+", " ").trim(); }
    private static String slug(String s) { String r = s.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9]+", "-").replaceAll("(^-|-$)", ""); return r.isEmpty() ? "mod" : r.length() > 40 ? r.substring(0, 40) : r; }

    static List<String> lines(File f) {
        List<String> out = new ArrayList<>(); if (!f.exists()) return out;
        try (BufferedReader r = new BufferedReader(new FileReader(f))) { for (String l; (l = r.readLine()) != null; ) if (!l.isEmpty() && !l.startsWith("#")) out.add(l); }
        catch (IOException ignored) {}
        return out;
    }

    static String read(File f) { try { return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); } catch (IOException e) { return ""; } }

    static void write(File f, String s) {
        f.getParentFile().mkdirs(); File tmp = new File(f.getPath() + ".tmp");
        try (OutputStream o = new FileOutputStream(tmp)) { o.write(s.getBytes(StandardCharsets.UTF_8)); } catch (IOException e) { return; }
        tmp.renameTo(f);
    }

    static void writeBytes(File f, byte[] b) throws IOException { try (OutputStream o = new FileOutputStream(f)) { o.write(b); } }

    private static void copy(InputStream in, OutputStream out) throws IOException { byte[] buf = new byte[1 << 16]; for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n); }

    private static void copyTree(File from, File to) throws IOException {
        if (from.isDirectory()) { to.mkdirs(); File[] fs = from.listFiles(); if (fs != null) for (File f : fs) copyTree(f, new File(to, f.getName())); }
        else try (InputStream in = new FileInputStream(from); OutputStream o = new FileOutputStream(to)) { copy(in, o); }
    }

    static void deleteTree(File f) {
        if (f.isDirectory()) { File[] fs = f.listFiles(); if (fs != null) for (File c : fs) deleteTree(c); }
        f.delete();
    }
}

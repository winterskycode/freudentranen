package dev.tok.totk;

import android.content.Context;
import android.os.Build;
import org.json.JSONException;
import org.json.JSONObject;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** GPU drivers the player installs from a zip, in the format emulators use (adrenotools packages: a meta.json naming the
    driver's library, the library and any libraries it loads beside it, all at the zip's top level). Each is unpacked to
    files/drivers/<id>/ and loaded by the game through adrenotools (vk/gpu_driver.c, DM_VK_DRIVER_EXPECT=any), with its
    own shader cache (files/shader-cache-zip-<id>). Such drivers are builds of Turnip or of Qualcomm's driver: they run on
    Adreno GPUs only, and other GPUs use their system driver. */
final class Drivers {
    static final class Driver {
        String id, name, description, author, version, vendor, driverVersion, library;
        File dir;
    }

    /** The selection (TOTK_GPU_DRIVER in the launcher's preferences): the bundled Turnip, the system's driver, or zip:<id>. */
    static final String TURNIP = "turnip", SYSTEM = "system", ZIP = "zip:";

    /** An Adreno GPU (Qualcomm's kernel GPU driver): the only GPUs Turnip and driver zips can drive. */
    static boolean adreno() { return new File("/dev/kgsl-3d0").exists(); }

    static File root(Context c) { return new File(c.getFilesDir(), "drivers"); }

    static List<Driver> list(Context c) {
        List<Driver> out = new ArrayList<>();
        File[] dirs = root(c).listFiles();
        if (dirs != null) for (File d : dirs) {
            try { Driver v = read(d); if (new File(d, v.library).isFile()) out.add(v); } catch (IOException | JSONException e) { /* half-installed: skipped */ }
        }
        out.sort((a, b) -> a.name.compareToIgnoreCase(b.name));
        return out;
    }

    static Driver find(Context c, String id) {
        for (Driver d : list(c)) if (d.id.equals(id)) return d;
        return null;
    }

    private static Driver read(File dir) throws IOException, JSONException {
        JSONObject j = new JSONObject(new String(readAll(new File(dir, "meta.json")), StandardCharsets.UTF_8));
        Driver d = new Driver();
        d.id = dir.getName(); d.dir = dir;
        d.name = j.optString("name", d.id); d.description = j.optString("description", ""); d.author = j.optString("author", "");
        d.version = j.optString("packageVersion", ""); d.vendor = j.optString("vendor", ""); d.driverVersion = j.optString("driverVersion", "");
        d.library = j.getString("libraryName");
        return d;
    }

    /** Unpacks a driver zip; the IOException's message says what is wrong with it. */
    static Driver install(Context c, File zip) throws IOException {
        File tmp = new File(root(c), ".install"); delete(tmp); tmp.mkdirs();
        try {
            int files = 0;
            try (ZipInputStream in = new ZipInputStream(new FileInputStream(zip))) {
                for (ZipEntry e; (e = in.getNextEntry()) != null; ) {
                    if (e.isDirectory()) continue;
                    String n = e.getName();
                    if (n.contains("/") || n.contains("\\") || n.equals("..") || n.startsWith(".")) continue;   // top level only, as adrenotools packages are
                    try (OutputStream out = new FileOutputStream(new File(tmp, n))) { copy(in, out); }
                    if (++files > 64) throw new IOException("It holds more files than a driver package does.");
                }
            }
            File meta = new File(tmp, "meta.json");
            if (!meta.isFile()) throw new IOException("It has no meta.json, so it is not a driver package (the zips made for emulators have one).");
            JSONObject j;
            try { j = new JSONObject(new String(readAll(meta), StandardCharsets.UTF_8)); } catch (JSONException e) { throw new IOException("Its meta.json cannot be read."); }
            String lib = j.optString("libraryName", "");
            if (lib.isEmpty() || lib.contains("/") || !lib.endsWith(".so")) throw new IOException("Its meta.json names no driver library.");
            File so = new File(tmp, lib);
            if (!so.isFile()) throw new IOException("The library " + lib + " that meta.json names is not in the zip.");
            if (!arm64(so)) throw new IOException(lib + " is not a 64-bit ARM library.");
            int minApi = j.optInt("minApi", 0);
            if (minApi > Build.VERSION.SDK_INT) throw new IOException("It needs Android API " + minApi + "; this device has " + Build.VERSION.SDK_INT + ".");
            String id = (slug(j.optString("name", "driver")) + "-" + sha256(so).substring(0, 8));
            File dir = new File(root(c), id);
            delete(dir);
            if (!tmp.renameTo(dir)) throw new IOException("It could not be stored.");
            try { return read(dir); } catch (JSONException e) { throw new IOException("Its meta.json cannot be read."); }
        } finally { delete(tmp); }
    }

    /** Removes the driver and the shader cache built with it. */
    static void remove(Context c, Driver d) {
        delete(d.dir);
        delete(new File(c.getFilesDir(), "shader-cache-zip-" + d.id));
    }

    /** ELF, 64-bit, AArch64 (e_machine 183). */
    private static boolean arm64(File f) throws IOException {
        byte[] h = new byte[20];
        try (InputStream in = new FileInputStream(f)) { if (in.read(h) != h.length) return false; }
        return h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' && h[4] == 2 && (h[18] & 0xff) == 183 && h[19] == 0;
    }

    private static String slug(String s) {
        String t = s.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9]+", "-").replaceAll("^-+|-+$", "");
        return t.isEmpty() ? "driver" : t.length() > 40 ? t.substring(0, 40) : t;
    }

    private static String sha256(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256"); byte[] b = new byte[1 << 16];
            for (int n; (n = in.read(b)) > 0; ) md.update(b, 0, n);
            StringBuilder s = new StringBuilder(); for (byte x : md.digest()) s.append(String.format(Locale.ROOT, "%02x", x));
            return s.toString();
        } catch (NoSuchAlgorithmException e) { throw new IOException(e); }
    }

    private static byte[] readAll(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) { java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream(); copy(in, out); return out.toByteArray(); }
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] b = new byte[1 << 16]; for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
    }

    private static void delete(File f) {
        File[] kids = f.listFiles(); if (kids != null) for (File k : kids) delete(k);
        f.delete();
    }
}

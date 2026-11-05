package dev.tok.totk;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import android.os.Build;
import android.provider.Settings;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Bug reports under /sdcard/tok/reports (written in game by host/report.c, or here after a crash): their recorded video
    segments (host/recorder.c format) become replay.mp4 and the paused frame frame.png; a report is shared as one zip. */
final class ReportStore {
    static File ROOT = new File("/sdcard/tok/reports");
    /** A player's app has no all-files access: the reports live in the app's storage and are shared from there. */
    static void context(android.content.Context c) { if (!android.os.Environment.isExternalStorageManager()) ROOT = new File(c.getFilesDir(), "reports"); }

    /** The reports, newest first. */
    static List<File> list() {
        File[] dirs = ROOT.listFiles(File::isDirectory);
        List<File> out = new ArrayList<>();
        if (dirs == null) return out;
        Arrays.sort(dirs, (a, b) -> b.getName().compareTo(a.getName()));
        out.addAll(Arrays.asList(dirs));
        return out;
    }

    static long size(File f) {
        if (f.isFile()) return f.length();
        long n = 0; File[] in = f.listFiles();
        if (in != null) for (File c : in) n += size(c);
        return n;
    }

    /** What still needs converting in a report: segments into replay.mp4, frame.ppm into frame.png (the raw files go). */
    static void finish(File dir) {
        File video = new File(dir, "video"), mp4 = new File(dir, "replay.mp4");
        File[] segments = video.listFiles((d, n) -> n.endsWith(".rec"));
        if (segments != null && segments.length > 0 && !mp4.exists()) {
            Arrays.sort(segments);
            File part = new File(dir, "replay.mp4.part");
            if (mux(segments, part) && part.renameTo(mp4)) { for (File s : segments) s.delete(); video.delete(); }
            else part.delete();
        }
        File ppm = new File(dir, "frame.ppm"), png = new File(dir, "frame.png");
        if (ppm.exists() && !png.exists()) {
            Bitmap b = Backdrop.decode(ppm);
            if (b != null) {
                try (OutputStream out = new FileOutputStream(png)) { if (b.compress(Bitmap.CompressFormat.PNG, 100, out)) ppm.delete(); }
                catch (IOException e) { png.delete(); }
            }
        }
    }

    static boolean pending(File dir) {
        File[] segments = new File(dir, "video").listFiles((d, n) -> n.endsWith(".rec"));
        return (segments != null && segments.length > 0) || new File(dir, "frame.ppm").exists();
    }

    /** Segments (records of size, flags, time, Annex-B H.264) into one MP4; a record cut short by a crash ends it. */
    private static boolean mux(File[] segments, File out) {
        MediaMuxer muxer = null; int track = -1; long last = Long.MIN_VALUE; int samples = 0;
        try {
            muxer = new MediaMuxer(out.getPath(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            for (File f : segments) {
                byte[] bytes = Files.readAllBytes(f.toPath());
                // whole: MediaMuxer reads array() from info.offset
                ByteBuffer b = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN), whole = ByteBuffer.wrap(bytes);
                if (b.remaining() < 16) continue;
                byte[] magic = new byte[8]; b.get(magic);
                if (!new String(magic, "US-ASCII").equals("TOTKREC1")) continue;
                int w = b.getInt(), h = b.getInt();
                while (b.remaining() >= 16) {
                    int size = b.getInt(), flags = b.getInt(); long pts = b.getLong();
                    if (size < 0 || size > b.remaining()) break;
                    int at = b.position(); ByteBuffer data = b.slice(); data.limit(size); b.position(at + size);
                    if ((flags & 2) != 0) {
                        if (track >= 0) continue;
                        MediaFormat format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, w, h);
                        ByteBuffer[] csd = parameterSets(data);
                        if (csd[0] == null || csd[1] == null) return false;
                        format.setByteBuffer("csd-0", csd[0]); format.setByteBuffer("csd-1", csd[1]);
                        track = muxer.addTrack(format); muxer.start();
                        continue;
                    }
                    if (track < 0 || pts <= last) continue;
                    // start at a key frame
                    if (samples == 0 && (flags & 1) == 0) continue;
                    MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                    info.set(at, size, pts, (flags & 1) != 0 ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0);
                    muxer.writeSampleData(track, whole, info); last = pts; samples++;
                }
            }
            if (track < 0 || samples == 0) return false;
            muxer.stop();
            return true;
        } catch (IOException | RuntimeException e) {
            return false;
        } finally {
            if (muxer != null) try { muxer.release(); } catch (RuntimeException ignored) { /* already failed */ }
        }
    }

    /** The SPS and PPS NAL units (with their start codes) of the encoder's configuration. */
    private static ByteBuffer[] parameterSets(ByteBuffer config) {
        byte[] d = new byte[config.remaining()]; config.duplicate().get(d);
        // start of the start code, start of the NAL
        List<int[]> units = new ArrayList<>();
        for (int i = 0; i + 3 <= d.length; i++)
            if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (i + 4 <= d.length && d[i + 2] == 0 && d[i + 3] == 1))) {
                int nal = d[i + 2] == 1 ? i + 3 : i + 4; units.add(new int[] {i, nal}); i = nal - 1;
            }
        ByteBuffer[] out = new ByteBuffer[2];
        for (int k = 0; k < units.size(); k++) {
            int start = units.get(k)[0], nal = units.get(k)[1], end = k + 1 < units.size() ? units.get(k + 1)[0] : d.length;
            int type = d[nal] & 0x1f;
            if (type == 7 && out[0] == null) out[0] = ByteBuffer.wrap(Arrays.copyOfRange(d, start, end));
            if (type == 8 && out[1] == null) out[1] = ByteBuffer.wrap(Arrays.copyOfRange(d, start, end));
        }
        return out;
    }

    private static final String FATAL = "[dm] FATAL ", DRIVER_FAILED = "[vulkan-loader] custom initialization failed: ", GPU = "[vulkan-loader] actual device=";

    /** Why the last run stopped, from its log: the driver loader's reason when an installed GPU driver failed to start
        (vk/gpu_driver.c), else the renderer's fatal error; null when it did not stop on one. */
    static String lastFailure(Context c) {
        String tail = tail(new File(c.getFilesDir(), "profile/totk.log"), 64 * 1024);
        if (!tail.contains(FATAL)) return null;
        int d = tail.lastIndexOf(DRIVER_FAILED);
        return d >= 0 ? line(tail, d + DRIVER_FAILED.length()) : line(tail, tail.lastIndexOf(FATAL) + FATAL.length());
    }

    /** True when the last run stopped because an installed GPU driver failed to start. */
    static boolean driverFailed(Context c) {
        String tail = tail(new File(c.getFilesDir(), "profile/totk.log"), 64 * 1024);
        return tail.contains(FATAL) && tail.contains(DRIVER_FAILED);
    }

    private static String line(String s, int from) { int end = s.indexOf('\n', from); return (end < 0 ? s.substring(from) : s.substring(from, end)).trim(); }

    /** The last run ended with a fatal signal or a runtime failure in its log: a report from its log, settings and the
        recorder's segments, once per run. Null when there is nothing new. */
    static File crashReport(Context c, SharedPreferences prefs) {
        File log = new File(c.getFilesDir(), "profile/totk.log");
        if (!log.exists() || prefs.getLong("crash_reported", 0) == log.lastModified()) return null;
        String tail = tail(log, 64 * 1024);
        if (!tail.contains("[totk] signal=") && !tail.contains("[totk] FAIL:") && !tail.contains(FATAL)) return null;
        prefs.edit().putLong("crash_reported", log.lastModified()).apply();
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date(log.lastModified()));
        File dir = new File(ROOT, stamp + "-crash");
        if (!dir.mkdirs() && !dir.isDirectory()) return null;
        copy(log, new File(dir, "totk.log"));
        for (String n : new String[] {"launch.env", "debug.env", "actions.txt"}) copy(new File(c.getFilesDir(), n), new File(dir, n));
        File[] segments = new File(c.getFilesDir(), "flight").listFiles((d, n) -> n.endsWith(".rec"));
        if (segments != null && segments.length > 0) {
            Arrays.sort(segments); File video = new File(dir, "video"); video.mkdirs();
            for (int i = Math.max(0, segments.length - 7); i < segments.length; i++) copy(segments[i], new File(video, segments[i].getName()));
        }
        try (OutputStream out = new FileOutputStream(new File(dir, "device.txt"))) {
            String text = "Crash report (made by the startup screen from the last run)\nDevice: " + Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE
                + "\nFan mode: " + Settings.System.getString(c.getContentResolver(), "fan_mode") + ", performance mode: " + Settings.System.getString(c.getContentResolver(), "performance_mode") + "\n";
            int gpu = tail.lastIndexOf(GPU); String why = lastFailure(c);
            if (gpu >= 0) text += "GPU: " + line(tail, gpu + GPU.length()) + "\n";
            if (why != null) text += "Stopped because: " + why + "\n";
            out.write(text.getBytes("UTF-8"));
        } catch (IOException ignored) { /* the rest of the report stands */ }
        return dir;
    }

    /** The report as dir.zip next to it (the share sheet's file). */
    static File zip(File dir) throws IOException {
        File zip = new File(dir.getParentFile(), dir.getName() + ".zip"), part = new File(zip.getPath() + ".part");
        try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(part))) {
            out.setLevel(1);
            add(out, dir, dir.getName());
        }
        if (!part.renameTo(zip)) throw new IOException("rename");
        return zip;
    }

    private static void add(ZipOutputStream out, File f, String name) throws IOException {
        if (f.isDirectory()) { File[] in = f.listFiles(); if (in != null) for (File c : in) add(out, c, name + "/" + c.getName()); return; }
        out.putNextEntry(new ZipEntry(name));
        try (InputStream in = new FileInputStream(f)) { byte[] b = new byte[1 << 16]; for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n); }
        out.closeEntry();
    }

    static void delete(File f) {
        File[] in = f.listFiles();
        if (in != null) for (File c : in) delete(c);
        f.delete();
    }

    private static void copy(File from, File to) {
        if (!from.exists()) return;
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
            byte[] b = new byte[1 << 16]; for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
        } catch (IOException ignored) { /* a missing piece of a report */ }
    }

    static String head(File f, int bytes) {
        try (InputStream in = new FileInputStream(f)) {
            byte[] b = new byte[bytes]; int n = 0, k; while (n < bytes && (k = in.read(b, n, bytes - n)) > 0) n += k;
            String s = new String(b, 0, n, java.nio.charset.StandardCharsets.UTF_8); int cut = s.lastIndexOf('\n');
            return n == bytes && cut > 0 ? s.substring(0, cut) : s;
        } catch (IOException e) { return ""; }
    }

    static String tail(File f, int bytes) {
        try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(f, "r")) {
            long n = r.length(), from = Math.max(0, n - bytes);
            byte[] b = new byte[(int) (n - from)]; r.seek(from); r.readFully(b);
            return new String(b, java.nio.charset.StandardCharsets.UTF_8);
        } catch (IOException e) { return ""; }
    }

    private ReportStore() {}
}

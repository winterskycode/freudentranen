package dev.tok.totk;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.Rect;
import android.graphics.Shader;
import android.view.View;
import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;

/** The picture behind every page: the game's frame from when its menu was last opened (files/backdrop.ppm, written by the
    Vulkan backend, nvn_vulkan.c backdrop_save), else a drawn sky. The startup page darkens its left side for the menu;
    the other pages dim all of it. */
final class Backdrop extends View {
    static Bitmap frame;   // decoded once per process, reread when the file changes
    private static long frameTime;
    private final boolean home;

    Backdrop(Context c, boolean home) { super(c); this.home = home; }

    /** The saved frame, if there is one (a file the game writes: decoded off the UI thread by the caller). */
    static synchronized Bitmap load(File f) {
        if (!f.exists()) return frame = null;
        if (frame != null && f.lastModified() == frameTime) return frame;
        Bitmap b = decode(f);
        if (b != null) { frame = b; frameTime = f.lastModified(); }
        return b;
    }

    /** A binary PPM (P6, 8 bits) as a bitmap; null when it is not one. */
    static Bitmap decode(File f) {
        try (InputStream in = new BufferedInputStream(new FileInputStream(f), 1 << 16)) {
            if (in.read() != 'P' || in.read() != '6') return null;
            int w = number(in), h = number(in), max = number(in);
            if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || max != 255) return null;
            byte[] rgb = new byte[w * h * 3]; int got = 0;
            while (got < rgb.length) { int n = in.read(rgb, got, rgb.length - got); if (n < 0) return null; got += n; }
            int[] px = new int[w * h];
            for (int i = 0; i < px.length; i++) px[i] = 0xFF000000 | (rgb[i * 3] & 0xFF) << 16 | (rgb[i * 3 + 1] & 0xFF) << 8 | (rgb[i * 3 + 2] & 0xFF);
            return Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
        } catch (IOException | OutOfMemoryError e) { return null; }
    }

    private static int number(InputStream in) throws IOException {   // a PPM header field (whitespace and # comments around it)
        int c = in.read();
        while (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '#') { if (c == '#') while (c != '\n' && c >= 0) c = in.read(); c = in.read(); }
        int v = 0; while (c >= '0' && c <= '9') { v = v * 10 + c - '0'; c = in.read(); }
        return v;
    }

    @Override protected void onDraw(Canvas c) {
        int w = getWidth(), h = getHeight();
        Bitmap b = frame;
        if (b != null) {   // centre crop
            float k = Math.max((float) w / b.getWidth(), (float) h / b.getHeight());
            int sw = Math.round(w / k), sh = Math.round(h / k), sx = (b.getWidth() - sw) / 2, sy = (b.getHeight() - sh) / 2;
            Paint p = new Paint(Paint.FILTER_BITMAP_FLAG);
            c.drawBitmap(b, new Rect(sx, sy, sx + sw, sy + sh), new Rect(0, 0, w, h), p);
        } else sky(c, w, h);
        Paint scrim = new Paint();
        if (home) {
            scrim.setShader(new LinearGradient(0, 0, w, 0, new int[] {0xF0080D0F, 0xD1080D0F, 0x33080D0F, 0x00080D0F},
                new float[] {0f, 0.34f, 0.66f, 1f}, Shader.TileMode.CLAMP));
            c.drawRect(0, 0, w, h, scrim);
            scrim.setShader(new LinearGradient(0, h, 0, h - Ui.u(120), 0xD9080D0F, 0x00080D0F, Shader.TileMode.CLAMP));
            c.drawRect(0, h - Ui.u(120), w, h, scrim);
        } else { scrim.setColor(0xD1070B0D); c.drawRect(0, 0, w, h, scrim); }
    }

    /** No frame yet: dusk over a sea of clouds with floating islands (drawn in the canvas's 1280 x 720 units). */
    private static void sky(Canvas c, int w, int h) {
        c.save();
        float k = Math.max(w / 1280f, h / 720f); c.translate((w - 1280 * k) / 2, (h - 720 * k) / 2); c.scale(k, k);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setShader(new LinearGradient(0, 0, 0, 720, new int[] {0xFF0E1D2E, 0xFF2B4A62, 0xFF9C8E7A, 0xFFE2B979, 0xFFF1D3A0},
            new float[] {0f, 0.45f, 0.72f, 0.86f, 1f}, Shader.TileMode.CLAMP));
        c.drawRect(0, 0, 1280, 720, p);
        p.setShader(new RadialGradient(922, 562, 450, new int[] {0xE6FFE8B8, 0x73F6C98A, 0x00F6C98A}, new float[] {0f, 0.25f, 1f}, Shader.TileMode.CLAMP));
        c.drawRect(0, 0, 1280, 720, p); p.setShader(null);
        p.setColor(0xD9EFE6D2); c.drawCircle(1010, 150, 46, p);
        p.setColor(0x8C22394E); c.drawCircle(1024, 142, 44, p);
        p.setColor(0xCC6B767A);
        c.drawPath(path("M150 420 q30 -14 70 -10 q40 -2 64 10 l-20 14 q-30 26 -50 46 q-16 -22 -40 -36 z"), p);
        c.drawPath(path("M520 360 q22 -8 46 -6 q24 0 36 8 l-14 10 q-16 18 -28 30 q-10 -16 -26 -26 z"), p);
        p.setColor(0xD9D8C1A0);
        c.drawPath(path("M0 520 q80 -36 170 -22 q70 -40 160 -12 q90 -46 200 -8 q80 -30 170 -4 q100 -40 210 -6 q90 -26 180 8 q60 -10 190 4 v240 h-1280 z"), p);
        p.setColor(0xFF222E2C);
        c.drawPath(path("M760 300 q60 -30 150 -26 q90 -6 150 22 q20 8 10 18 l-40 18 q-30 40 -70 70 q-16 30 -40 52 q-14 -26 -40 -40 q-40 -24 -64 -64 l-50 -30 q-16 -10 -6 -20 z"), p);
        c.drawPath(path("M300 250 q40 -18 92 -14 q46 0 74 16 l-20 14 q-24 34 -44 56 q-12 -20 -32 -30 q-28 -18 -50 -32 z"), p);
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(3); p.setColor(0xBF9FF2D6);
        c.drawCircle(900, 262, 34, p); c.drawCircle(900, 262, 22, p);
        c.drawLine(900, 228, 900, 210, p); c.drawLine(900, 296, 900, 314, p); c.drawLine(866, 262, 848, 262, p); c.drawLine(934, 262, 952, 262, p);
        p.setStyle(Paint.Style.FILL); p.setColor(0x149FF2D6); c.drawCircle(900, 262, 60, p);
        p.setShader(new LinearGradient(0, 560, 0, 720, 0xFFF4E3C6, 0xFFB9B6AE, Shader.TileMode.CLAMP));
        c.drawPath(path("M0 600 q90 -40 200 -20 q80 -36 190 -10 q110 -40 230 -6 q100 -34 210 0 q120 -40 240 -4 q120 -20 210 14 v146 h-1280 z"), p);
        p.setShader(null); p.setColor(0xE6F7EBD4);
        c.drawPath(path("M0 660 q120 -30 260 -10 q120 -24 260 0 q140 -26 280 0 q140 -22 260 2 q110 -16 220 10 v68 h-1280 z"), p);
        c.restore();
    }

    /** The few SVG path commands the sky uses: M (absolute), and relative q, l, v, h, z. */
    private static Path path(String d) {
        Path p = new Path(); String[] t = d.trim().split("\\s+"); int i = 0; char cmd = 'M';
        while (i < t.length) {
            String s = t[i]; char c0 = s.charAt(0);
            if (Character.isLetter(c0)) { cmd = c0; s = s.substring(1); if (s.isEmpty()) { i++; if (cmd == 'z') { p.close(); continue; } s = t[i]; } }
            float a = Float.parseFloat(s);
            switch (cmd) {
                case 'M': p.moveTo(a, Float.parseFloat(t[++i])); break;
                case 'l': p.rLineTo(a, Float.parseFloat(t[++i])); break;
                case 'q': p.rQuadTo(a, Float.parseFloat(t[i + 1]), Float.parseFloat(t[i + 2]), Float.parseFloat(t[i + 3])); i += 3; break;
                case 'v': p.rLineTo(0, a); break;
                case 'h': p.rLineTo(a, 0); break;
                default: break;
            }
            i++;
        }
        return p;
    }
}

package dev.tok.totk;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.util.DisplayMetrics;

/** The look of the startup screen (vk/overlay.cpp draws the same in game): colours, the two typefaces (assets/fonts, SIL
    OFL) and the ornaments. Sizes are design units of a 1280 x 720 frame, times U. */
final class Ui {
    static final int INK = 0xFF0A1114, CREAM = 0xFFEEE8D8, MUTED = 0xFFA7AEA2, SOFT = 0xFFCFC9B8, HELP = 0xFFD6D0C0;
    static final int GOLD = 0xFFD8B45C, GOLD_TEXT = 0xFFEBCB7C, GOLD_FRAME = 0xB2D8B45C, GOLD_FILL = 0x1AD8B45C;
    static final int JADE = 0xFF6FE6C3, OCHRE = 0xFFE59A72, OCHRE_TEXT = 0xFFF0B494, LINE = 0x24EEE8D8, PANEL = 0xDC0C1417;

    static Typeface title, body, medium, semibold;
    static float U = 1.5f;

    static void init(Context c) {
        if (title == null) {
            title = font(c, "fonts/MarcellusSC-Regular.ttf", Typeface.SERIF);
            body = font(c, "fonts/Figtree-Regular.ttf", Typeface.SANS_SERIF);
            medium = font(c, "fonts/Figtree-Medium.ttf", body);
            semibold = font(c, "fonts/Figtree-SemiBold.ttf", body);
        }
        DisplayMetrics m = c.getResources().getDisplayMetrics();
        U = Math.min(Math.max(m.widthPixels, m.heightPixels) / 1280f, Math.min(m.widthPixels, m.heightPixels) / 720f);
    }

    private static Typeface font(Context c, String asset, Typeface fallback) {
        try { return Typeface.createFromAsset(c.getAssets(), asset); } catch (RuntimeException e) { return fallback; }
    }

    static float u(float v) { return v * U; }
    static int ui(float v) { return Math.round(v * U); }

    static Paint text(Typeface face, float size, int color) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
        p.setTypeface(face); p.setTextSize(u(size)); p.setColor(color);
        return p;
    }

    static Paint fill(int color) { Paint p = new Paint(Paint.ANTI_ALIAS_FLAG); p.setColor(color); return p; }

    static Paint stroke(int color, float width) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG); p.setColor(color); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(u(width));
        return p;
    }

    /** Text drawn with its vertical middle at y. */
    static void centred(Canvas c, String s, float x, float y, Paint p) {
        Paint.FontMetrics f = p.getFontMetrics();
        c.drawText(s, x, y - (f.ascent + f.descent) / 2, p);
    }

    static void diamond(Canvas c, float x, float y, float r, Paint p) {
        Path d = new Path(); d.moveTo(x, y - r); d.lineTo(x + r, y); d.lineTo(x, y + r); d.lineTo(x - r, y); d.close();
        c.drawPath(d, p);
    }

    /** A tick with its middle at (x, y). */
    static void tick(Canvas c, float x, float y, float r, int color) {
        Path t = new Path(); t.moveTo(x - r, y); t.lineTo(x - r / 4, y + r * 0.75f); t.lineTo(x + r, y - r * 0.75f);
        Paint p = stroke(color, 2.5f); p.setStrokeCap(Paint.Cap.ROUND); p.setStrokeJoin(Paint.Join.ROUND);
        c.drawPath(t, p);
    }

    /** A gold rule with a diamond in the middle. */
    static void ornament(Canvas c, float x, float y, float w) {
        Paint line = stroke(0x99D8B45C, 1); float m = x + w / 2;
        c.drawLine(x, y, m - u(8), y, line); c.drawLine(m + u(8), y, x + w, y, line);
        diamond(c, m, y, u(4), fill(GOLD));
    }

    /** The selection: a hairline gold frame on a faint gold fill, and the diamond cursor at the left. */
    static void selection(Canvas c, float l, float t, float r, float b) {
        RectF box = new RectF(l + u(0.5f), t + u(0.5f), r - u(0.5f), b - u(0.5f));
        c.drawRoundRect(box, u(4), u(4), fill(GOLD_FILL));
        c.drawRoundRect(box, u(4), u(4), stroke(GOLD_FRAME, 1));
        diamond(c, l + u(18), (t + b) / 2, u(4.5f), fill(GOLD));
    }

    /** A panel: dark fill, hairline and gold corner brackets. */
    static void panel(Canvas c, float l, float t, float r, float b) {
        c.drawRect(l, t, r, b, fill(PANEL));
        c.drawRect(l, t, r, b, stroke(LINE, 1));
        Paint g = stroke(GOLD, 2); float k = u(16), e = u(1);
        c.drawLine(l - e, t, l + k, t, g); c.drawLine(l, t - e, l, t + k, g);
        c.drawLine(r + e, t, r - k, t, g); c.drawLine(r, t - e, r, t + k, g);
        c.drawLine(l - e, b, l + k, b, g); c.drawLine(l, b + e, l, b - k, g);
        c.drawLine(r + e, b, r - k, b, g); c.drawLine(r, b + e, r, b - k, g);
    }

    /** A footer hint: a cream chip with the button and the words after it; returns the x after it. */
    static float hint(Canvas c, float x, float y, String glyph, String what) {
        Paint g = text(semibold, 12, INK), t = text(body, 14, SOFT);
        float h = u(24), w = glyph.length() > 1 ? g.measureText(glyph) + u(14) : h;
        if (glyph.length() > 1) c.drawRoundRect(new RectF(x, y, x + w, y + h), u(6), u(6), fill(CREAM));
        else c.drawCircle(x + h / 2, y + h / 2, h / 2, fill(CREAM));
        g.setTextAlign(Paint.Align.CENTER); centred(c, glyph, x + w / 2, y + h / 2, g);
        centred(c, what, x + w + u(8), y + h / 2, t);
        return x + w + u(8) + t.measureText(what) + u(22);
    }

    /** An outlined tag (RESTART, EXPERIMENTAL) ending at right; returns its left edge. */
    static float tag(Canvas c, float right, float mid, String s, int line, int ink) {
        Paint t = text(semibold, 11, ink); t.setLetterSpacing(0.08f);
        float w = t.measureText(s) + u(16), h = u(20);
        c.drawRoundRect(new RectF(right - w, mid - h / 2, right, mid + h / 2), u(3), u(3), stroke(line, 1));
        centred(c, s, right - w + u(8), mid, t);
        return right - w;
    }

    /** "‹ value ›" ending at right; returns its left edge. */
    static float value(Canvas c, float right, float mid, String v, int color) {
        Paint chev = text(body, 17, MUTED), val = text(medium, 17, color);
        float cw = chev.measureText("›"), x = right - cw;
        centred(c, "›", x, mid, chev);
        x -= u(10) + val.measureText(v); centred(c, v, x, mid, val);
        x -= u(10) + cw; centred(c, "‹", x, mid, chev);
        return x;
    }

    private Ui() {}
}

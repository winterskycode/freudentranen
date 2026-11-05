#!/usr/bin/env python3
"""The drawings of the PlayStation button icons, as one asset of the APK (IconMod.java puts them into the player's own files).

    python make_icon_asset.py <NotoSansCJKsc-Bold.otf> <out>   (tools/fetch_font.sh runs it, with its pinned font and fontTools)

Nothing here reads the game. The shapes are MacNativeGames' scripts/make_button_icons.py (MIT): playstation_style by
position, its d-pad from xbox_style, stick, rasterize, with the same numbers. The letters on keys (L1 R1 L2 R2 L3), which that
script cuts with the game's text fonts, come from Noto Sans CJK Bold (SIL OFL 1.1). This copy needs only fontTools: the script
needs Python 3.14, which the release machine lacks, and an upstream change there could not stop a build here.
tools/icon_test/compare.py checks both against the PC tool's result.

File, little endian: "TKICONS1", u16 glyphs, u16 textures. A glyph: u16 code point, u16 advance, i16 left side bearing,
u16 length, its Type 2 charstring without the width (the width counts from the font's nominalWidthX, so the device adds it).
A texture: u16 size, u8 name length, name, size * size bytes of coverage, rows top down.
"""
import math, struct, sys
from types import SimpleNamespace

from fontTools.pens.basePen import BasePen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.recordingPen import RecordingPen
from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

# Units of the game's icon font: a 1000-unit em, the round icons fill y 20..585
Y0, Y1 = 20, 585
CY, R = (Y0 + Y1) / 2, (Y1 - Y0) / 2
K = 0.5523
# the button guide's textures in 1.4.3
SIZE, MARGIN, SUPERSAMPLE = 108, 4, 4
TEXTURES = {'Nt_KeyTexA_00': 0xE040, 'Nt_KeyTexB_00': 0xE041, 'Nt_KeyTexX_00': 0xE042, 'Nt_KeyTexY_00': 0xE043,
            'Nt_KeyTexL_00': 0xE044, 'Nt_KeyTexZL_00': 0xE046,
            # the left stick with an arrow up: push it forward
            'Nt_KeyTexLSt_00': 0xE052,
            # the left stick
            'Nt_KeyTexLSt_01': 0xE05E}


class Letters:
    """Outlines of letters from an open font, drawn as holes in a shape."""
    def __init__(self, font):
        self.cmap, self.glyphs, self.upm = font.getBestCmap(), font.getGlyphSet(), font['head'].unitsPerEm

    def outline(self, ch):
        g = self.glyphs[self.cmap[ord(ch)]]
        rec = RecordingPen(); g.draw(rec); box = BoundsPen(self.glyphs); g.draw(box)
        return rec, box.bounds, g.width

    def hole(self, pen, text, cx, cy, cap, max_width):
        parts, x, ink, k = [], 0.0, [1e9, 1e9, -1e9, -1e9], 1000 / self.upm
        for ch in text:
            rec, b, advance = self.outline(ch)
            parts.append((rec, x))
            ink = [min(ink[0], x + b[0] * k), min(ink[1], b[1] * k), max(ink[2], x + b[2] * k), max(ink[3], b[3] * k)]
            x += advance * k * 0.96
        width, height = ink[2] - ink[0], ink[3] - ink[1]
        scale = min(cap / height, max_width / width)
        ox, oy = cx - (ink[0] + width / 2) * scale, cy - (ink[1] + height / 2) * scale
        reverse = ReverseContourPen(pen)
        for rec, x in parts:
            rec.replay(TransformPen(reverse, (scale * k, 0, 0, scale * k, ox + x * scale, oy)))


def circle(pen, cx, cy, r):
    k = K * r
    pen.moveTo((cx + r, cy))
    pen.curveTo((cx + r, cy + k), (cx + k, cy + r), (cx, cy + r))
    pen.curveTo((cx - k, cy + r), (cx - r, cy + k), (cx - r, cy))
    pen.curveTo((cx - r, cy - k), (cx - k, cy - r), (cx, cy - r))
    pen.curveTo((cx + k, cy - r), (cx + r, cy - k), (cx + r, cy))
    pen.closePath()


def rounded(pen, x0, y0, x1, y1, bottom, top=None):
    """Counter-clockwise rounded rectangle (bottom and top corner radii)."""
    top = bottom if top is None else top
    kb, kt = K * bottom, K * top
    pen.moveTo((x0 + bottom, y0)); pen.lineTo((x1 - bottom, y0))
    pen.curveTo((x1 - bottom + kb, y0), (x1, y0 + bottom - kb), (x1, y0 + bottom)); pen.lineTo((x1, y1 - top))
    pen.curveTo((x1, y1 - top + kt), (x1 - top + kt, y1), (x1 - top, y1)); pen.lineTo((x0 + top, y1))
    pen.curveTo((x0 + top - kt, y1), (x0, y1 - top + kt), (x0, y1 - top)); pen.lineTo((x0, y0 + bottom))
    pen.curveTo((x0, y0 + bottom - kb), (x0 + bottom - kb, y0), (x0 + bottom, y0)); pen.closePath()


def polygon(pen, points):
    pen.moveTo(points[0])
    for p in points[1:]:
        pen.lineTo(p)
    pen.closePath()


def oriented(pen, points, hole):
    """A polygon counter-clockwise (filled) or clockwise (a hole), whatever order the points come in."""
    area = sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(points, points[1:] + points[:1]))
    polygon(pen, points if (area > 0) != hole else points[::-1])


def arrow_up_hole(pen, cx, cy, size):
    """An arrow up (triangle head and stem) cut out of a key, clockwise so it is a hole."""
    # u along the arrow, v across it
    def at(u, v):
        return (cx - v * size, cy + u * size)
    pts = [at(0.5, 0), at(0.0, 0.42), at(0.0, 0.15), at(-0.5, 0.15), at(-0.5, -0.15), at(0.0, -0.15), at(0.0, -0.42)]
    polygon(ReverseContourPen(pen), pts)


def stick(g, label, up=False, advance=640):
    """The left stick as a DualSense's L3 in a circle; up: with an arrow up, the button guide's "push it forward"."""
    def draw(pen):
        cx = advance / 2; circle(pen, cx, CY, R)
        if up:
            g.hole(pen, label, cx, CY - R * 0.25, R * 0.55, R * 1.1); arrow_up_hole(pen, cx, CY + R * 0.5, R * 0.6)
        else:
            g.hole(pen, label, cx, CY, R * 0.95, R * 1.3)
    return advance, draw


def dpad(*lit, advance=640):
    def draw(pen):
        cx, length, arm = advance / 2, R, R * 0.40
        polygon(pen, [(cx + length, CY - arm), (cx + length, CY + arm), (cx + arm, CY + arm), (cx + arm, CY + length),
                      (cx - arm, CY + length), (cx - arm, CY + arm), (cx - length, CY + arm), (cx - length, CY - arm),
                      (cx - arm, CY - arm), (cx - arm, CY - length), (cx + arm, CY - length), (cx + arm, CY - arm)])
        # arm centre, half base and height of the arrow
        m, t, h = (length + arm) / 2, arm * 0.72, arm * 0.80
        heads = {'up': [(cx - t, CY + m - h / 2), (cx, CY + m + h / 2), (cx + t, CY + m - h / 2)],
                 'down': [(cx + t, CY - m + h / 2), (cx, CY - m - h / 2), (cx - t, CY - m + h / 2)],
                 'left': [(cx - m + h / 2, CY - t), (cx - m - h / 2, CY), (cx - m + h / 2, CY + t)],
                 'right': [(cx + m - h / 2, CY + t), (cx + m + h / 2, CY), (cx + m - h / 2, CY - t)]}
        for d in lit:
            # already clockwise: a hole
            polygon(pen, heads[d])
    return advance, draw


def playstation(g):
    """DualSense symbols by the Switch's button positions: A B X Y = Circle Cross Triangle Square (make_icon_mod.py; host/app.c
    shows the prompts with the Switch's letter positions, so the glyph of A is the east button's)."""
    def face(symbol, advance=640):
        def draw(pen):
            # symbol half size
            cx, s = advance / 2, R * 0.50
            circle(pen, cx, CY, R)
            if symbol == 'circle':
                circle(ReverseContourPen(pen), cx, CY, s); circle(pen, cx, CY, s * 0.70)
            elif symbol == 'square':
                oriented(pen, [(cx - s, CY - s), (cx + s, CY - s), (cx + s, CY + s), (cx - s, CY + s)], hole=True)
                t = s * 0.70
                oriented(pen, [(cx - t, CY - t), (cx + t, CY - t), (cx + t, CY + t), (cx - t, CY + t)], hole=False)
            elif symbol == 'triangle':
                pts = [(cx, CY + s * 1.10), (cx - s * 1.10, CY - s * 0.75), (cx + s * 1.10, CY - s * 0.75)]
                gx, gy = cx, sum(y for _, y in pts) / 3
                oriented(pen, pts, hole=True)
                oriented(pen, [(gx + (x - gx) * 0.62, gy + (y - gy) * 0.62) for x, y in pts], hole=False)
            # cross: one 12-point outline, so the two strokes do not overlap as separate holes
            else:
                # arm reach along the diagonal, half stroke width
                a, w = s * 1.05, s * 0.22 * math.sqrt(2)
                pts = [(cx, CY + w), (cx + a - w, CY + a), (cx + a, CY + a - w), (cx + w, CY),
                       (cx + a, CY - a + w), (cx + a - w, CY - a), (cx, CY - w), (cx - a + w, CY - a),
                       (cx - a, CY - a + w), (cx - w, CY), (cx - a, CY + a - w), (cx - a + w, CY + a)]
                oriented(pen, pts, hole=True)
        return advance, draw
    def bumper(label, advance=820):
        def draw(pen):
            rounded(pen, 40, Y0 + 70, advance - 40, Y1 - 70, 70, 150); g.hole(pen, label, advance / 2, CY, 230, advance - 200)
        return advance, draw
    def trigger(label, advance=760):
        def draw(pen):
            rounded(pen, 60, Y0, advance - 60, Y1, 60, 230); g.hole(pen, label, advance / 2, CY - 20, 230, advance - 220)
        return advance, draw
    # three bars, as on the DualSense Options button
    def options(advance=640):
        def draw(pen):
            circle(pen, advance / 2, CY, R); reverse = ReverseContourPen(pen)
            for dy in (-115, 0, 115):
                rounded(reverse, advance / 2 - 150, CY + dy - 38, advance / 2 + 150, CY + dy + 38, 38)
        return advance, draw
    # three short strokes rising to the right, as on the Create button
    def create(advance=640):
        def draw(pen):
            circle(pen, advance / 2, CY, R); cx = advance / 2
            for dx in (-110, 0, 110):
                oriented(pen, [(cx + dx - 30, CY - 150), (cx + dx + 30, CY - 150), (cx + dx + 30, CY + 150), (cx + dx - 30, CY + 150)], hole=True)
        return advance, draw
    return {
        0xE040: face('circle'), 0xE041: face('cross'), 0xE042: face('triangle'), 0xE043: face('square'),
        0xE044: bumper('L1'), 0xE045: bumper('R1'), 0xE046: trigger('L2'), 0xE047: trigger('R2'),
        0xE04B: options(), 0xE04C: create(),
        0xE052: stick(g, 'L3', up=True), 0xE05E: stick(g, 'L3'),
        0xE049: dpad('up', 'down', 'left', 'right'), 0xE060: dpad('up'), 0xE061: dpad('down'),
        0xE062: dpad('left'), 0xE063: dpad('right'), 0xE064: dpad('up', 'down'), 0xE065: dpad('left', 'right'),
    }


class PolygonPen(BasePen):
    """Outlines flattened to closed polygons."""
    def __init__(self):
        super().__init__(None); self.polygons = []

    def _moveTo(self, pt):
        self.polygons.append([pt])

    def _lineTo(self, pt):
        self.polygons[-1].append(pt)

    def _curveToOne(self, pt1, pt2, pt3):
        p0 = self.polygons[-1][-1]
        for i in range(1, 17):
            t = i / 16; u = 1 - t
            self.polygons[-1].append(tuple(u * u * u * p0[k] + 3 * u * u * t * pt1[k] + 3 * u * t * t * pt2[k] + t * t * t * pt3[k] for k in (0, 1)))


def rasterize(draw, size):
    """A size x size coverage mask (0..255, rows top down) of `draw`'s outline, fitted into the square, nonzero winding."""
    rec = RecordingPen(); draw(rec)
    box = BoundsPen(None); rec.replay(box); x0, y0, x1, y1 = box.bounds
    inner = size - 2 * MARGIN; scale = inner / max(x1 - x0, y1 - y0)
    ox = MARGIN + (inner - (x1 - x0) * scale) / 2 - x0 * scale
    oy = MARGIN + (inner - (y1 - y0) * scale) / 2 - y0 * scale
    # font y up -> rows down
    flat = PolygonPen(); rec.replay(TransformPen(flat, (scale, 0, 0, -scale, ox, size - oy)))
    edges = [(a, b) for poly in flat.polygons for a, b in zip(poly, poly[1:] + poly[:1]) if a[1] != b[1]]
    n = size * SUPERSAMPLE; cover = [0] * (size * size)
    for row in range(n):
        y = (row + 0.5) / SUPERSAMPLE
        crossings = sorted((a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]), 1 if b[1] > a[1] else -1)
                           for a, b in edges if min(a[1], b[1]) <= y < max(a[1], b[1]))
        winding, start = 0, None
        for x, d in crossings:
            if winding == 0:
                start = x
            winding += d
            if winding == 0:
                for col in range(max(0, round(start * SUPERSAMPLE)), min(n, round(x * SUPERSAMPLE))):
                    cover[(row // SUPERSAMPLE) * size + col // SUPERSAMPLE] += 1
    return bytes(min(255, c * 255 // (SUPERSAMPLE * SUPERSAMPLE)) for c in cover)


def charstring(draw):
    """The outline's Type 2 charstring with no width, and its left side bearing."""
    pen = T2CharStringPen(None, None); draw(pen)
    cs = pen.getCharString(); cs.compile(); code = bytes(cs.bytecode)
    cs.private = SimpleNamespace(nominalWidthX=0, defaultWidthX=0)
    box = BoundsPen(None); cs.draw(box)
    return code, int(round(box.bounds[0]))


def main():
    source, out = sys.argv[1:3]
    icons = playstation(Letters(TTFont(source, lazy=True)))
    data = bytearray(b'TKICONS1' + struct.pack('<HH', len(icons), len(TEXTURES)))
    for cp, (advance, draw) in sorted(icons.items()):
        code, lsb = charstring(draw)
        data += struct.pack('<HHhH', cp, advance, lsb, len(code)) + code
    for name, cp in TEXTURES.items():
        data += struct.pack('<HB', SIZE, len(name)) + name.encode() + rasterize(icons[cp][1], SIZE)
    with open(out, 'wb') as f:
        f.write(data)


if __name__ == '__main__':
    main()

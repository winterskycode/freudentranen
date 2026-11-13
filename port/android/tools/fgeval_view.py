#!/usr/bin/env python3
"""fgeval_view.py: the frame generation quality test's dumps (TOTK_FG_EVAL_DUMP, bench.py --fgeval N) as pictures.

    python3 port/android/tools/fgeval_view.py <dump folder> [--worst N] [--out folder]

The game writes, every N-th comparison, <frame>-eval-generated.ppm, <frame>-eval-error.ppm and <frame>-eval-real.r11g11b10
(the real frame's scene: packed unsigned floats, linear). Each triplet becomes one PNG, real | generated | error map
(grey: error x 4; red: pixels off by more than 24), and the folder's comparisons are ranked by their share of red pixels;
--worst keeps the N worst. Pull the dumps with: adb exec-out run-as dev.tok.totk sh -c 'cd files/fgdump && tar cf - .'
"""
import argparse, subprocess, sys
from pathlib import Path

import numpy as np


def ppm(path):
    data = path.read_bytes(); parts = data.split(b'\n', 3); w, h = map(int, parts[1].split())
    return np.frombuffer(parts[3], np.uint8)[:w * h * 3].reshape(h, w, 3)


def unsigned_float(bits, mantissa_bits):
    e = (bits >> mantissa_bits) & 31; m = (bits & ((1 << mantissa_bits) - 1)) / (1 << mantissa_bits)
    return np.where(e == 0, m * 2.0 ** -14, (1 + m) * 2.0 ** (e.astype(np.float64) - 15))


def r11g11b10(path, w, h):
    v = np.fromfile(path, np.uint32)[:w * h].reshape(h, w)
    rgb = np.stack([unsigned_float(v & 0x7ff, 6), unsigned_float((v >> 11) & 0x7ff, 6), unsigned_float((v >> 22) & 0x3ff, 5)], -1).clip(0, 1)
    srgb = np.where(rgb <= 0.0031308, rgb * 12.92, 1.055 * rgb ** (1 / 2.4) - 0.055)
    return (srgb * 255 + 0.5).astype(np.uint8)


def write_png(pixels, path):
    h, w = pixels.shape[:2]
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{w}x{h}', '-i', '-', str(path)],
                   input=pixels.tobytes(), check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('folder', type=Path)
    ap.add_argument('--worst', type=int, default=0, help='only the N comparisons with the most badly wrong pixels')
    ap.add_argument('--out', type=Path, help='where the PNGs go (default: the dump folder)')
    args = ap.parse_args()
    out = args.out or args.folder; out.mkdir(parents=True, exist_ok=True)
    ranked = []
    for err in sorted(args.folder.glob('*-eval-error.ppm')):
        base = err.name[:-len('-eval-error.ppm')]
        e = ppm(err)
        ranked.append((float(((e[..., 0] == 255) & (e[..., 1] < 255)).mean() * 100), base))
    ranked.sort(reverse=True)
    for share, base in ranked[:args.worst or len(ranked)]:
        e = ppm(args.folder/f'{base}-eval-error.ppm'); g = ppm(args.folder/f'{base}-eval-generated.ppm'); h, w = e.shape[:2]
        r = r11g11b10(args.folder/f'{base}-eval-real.r11g11b10', w, h)
        write_png(np.concatenate([r, g, e], 1), out/f'{base}-eval.png')
        print(f'{base}: {share:.2f}% of pixels off by more than 24 -> {out}/{base}-eval.png')


if __name__ == '__main__':
    sys.exit(main())

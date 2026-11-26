#!/usr/bin/env python3
"""Validate an NHDX pack the way hd_textures.c init()/read_pixels() do; decode-check NHD2 level 0."""
import argparse, re, struct, sys
from pathlib import Path


def levels_bytes(c, w, h, n):
    t = 0
    for _ in range(n):
        t += ((w + 3) // 4) * ((h + 3) // 4) * (8 if c == 0x0C else 16); w, h = max(1, w // 2), max(1, h // 2)
    return t


def main(folder):
    folder = Path(folder)
    data = (folder / 'manifest.bin').read_bytes()
    magic, ver, count, size = struct.unpack_from('<4sIII', data)
    assert magic == b'NHDX' and ver == 1 and 0 < count <= 4096 and size == 84 and len(data) == 16 + 84 * count
    prev, bad = -1, 0
    for i in range(count):
        r = data[16 + i * 84:16 + (i + 1) * 84]
        c, sw, sh, sb, sample, w, h, lo, hi = struct.unpack_from('<9I', r)
        name = r[36:].split(b'\0')[0].decode()
        assert sample >= prev, 'manifest not sorted'; prev = sample
        assert 0 < w <= 4096 and 0 < h <= 4096 and w * h <= 8 << 20 and re.fullmatch(r'[0-9a-f]{32}\.(dxtc|rgba)', name), name
        f = (folder / name).read_bytes()
        mg, fw, fh, nb = struct.unpack_from('<4sIII', f)
        ok = (fw, fh) == (w, h)
        if mg == b'NHD2':
            col, lv = struct.unpack_from('<II', f, 16)
            full = max(w, h).bit_length()
            ok = ok and col in (0x0C, 0x0E, 0x0F) and 0 < lv <= full and nb == levels_bytes(col, w, h, lv) and len(f) == 24 + nb
        elif mg == b'NHD1':
            ok = ok and nb == w * h * 4 and len(f) == 16 + nb
        else:
            ok = False
        if not ok:
            bad += 1; print('BAD', name, mg, fw, fh, w, h)
    print(f'{folder}: {count} records, {bad} bad')
    return 1 if bad else 0


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('pack', type=Path, help='pack folder with manifest.bin')
    sys.exit(main(ap.parse_args().pack))

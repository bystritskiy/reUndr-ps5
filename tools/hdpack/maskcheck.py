#!/usr/bin/env python3
"""Colour under low alpha: does each HD entry keep the original RGB where the original alpha < 64 (0.25)?

Road/floor textures use alpha as a reflection mask, not transparency: the colour under alpha~0 is drawn.
Per record with a known Xbox original (src/<key>.png in a work dir) and an HD file in BC2/BC3: every mip
level and the original are box-downscaled per channel (no premultiply) to the smaller size; RGB MAE over
texels whose original alpha < 64. Read-only; writes a JSON report.
"""
import argparse, json, struct, sys
from pathlib import Path
import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from finish import read_manifest  # noqa: E402

LOW = 64          # original alpha below this = "mask off" texels whose colour must survive
MAE_MAX = 12.0    # per channel, 0..255
MIN_SHARE = 0.01  # ignore textures with fewer such texels
MIN_SIDE = 16     # mips smaller than this are a few screen pixels; BC endpoints there are fit to mixed blocks


def levels(path):
    f = path.read_bytes()
    magic, W, H, n, color, lv = struct.unpack_from('<4sIIIII', f)
    if magic != b'NHD2' or color not in (0x0E, 0x0F):
        return None
    out, at, w, h = [], 24, W, H
    for _ in range(lv):
        size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16
        im = Image.frombytes('RGBA', (max(4, w), max(4, h)), f[at:at + size], 'bcn',
                             (2, 'DXT3') if color == 0x0E else (3, 'DXT5')).crop((0, 0, w, h))
        out.append(im); at += size; w, h = max(1, w // 2), max(1, h // 2)
    return out


def straight(im, size):
    return np.dstack([np.asarray(c.resize(size, Image.Resampling.BOX), np.float32) for c in im.split()])


def check(orig, hd_levels):
    worst = []
    for i, lv in enumerate(hd_levels):
        size = (min(orig.width, lv.width), min(orig.height, lv.height))
        if min(size) < MIN_SIDE:
            break
        o, d = straight(orig, size), straight(lv, size)
        m = o[..., 3] < LOW
        if m.mean() < MIN_SHARE:
            worst.append(None); continue
        diff = d[..., :3][m] - o[..., :3][m]
        worst.append({'level': i, 'mae': round(float(np.abs(diff).mean()), 2),
                      'luma_shift': round(float((diff @ [0.299, 0.587, 0.114]).mean()), 2), 'share': round(float(m.mean()), 3)})
    return worst


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('pack', type=Path)
    ap.add_argument('--works', nargs='+', type=Path, required=True, help='work dirs with src/<key>.png')
    ap.add_argument('--labels', nargs='*', default=None, help='report labels (pack/LABEL.json) for names')
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    src = {}
    for w in a.works:
        for p in (w / 'src').glob('*.png'):
            src.setdefault(p.stem, p)
    names = {}
    for j in a.pack.glob('*.json'):
        try:
            for t in json.loads(j.read_text(encoding='utf-8')).get('textures', []):
                names.setdefault(t['key'], t['names'])
        except (ValueError, AttributeError, KeyError, TypeError):
            pass
    rows, stats = [], {'records': 0, 'no_source': 0, 'not_bc23': 0, 'no_mask': 0, 'ok': 0, 'fail': 0}
    for r in read_manifest(a.pack):
        c, sw, sh, sb, s, w, h, lo, hi = struct.unpack_from('<9I', r)
        key = f"{c:02x}_{sw}x{sh}_{s:08x}_{lo | hi << 32:016x}"
        file = r[36:].split(b'\0')[0].decode()
        stats['records'] += 1
        if key not in src:
            stats['no_source'] += 1; continue
        lv = levels(a.pack / file)
        if lv is None:
            stats['not_bc23'] += 1; continue
        res = [x for x in check(Image.open(src[key]).convert('RGBA'), lv) if x]
        if not res:
            stats['no_mask'] += 1; continue
        bad = [x for x in res if x['mae'] > MAE_MAX]
        stats['fail' if bad else 'ok'] += 1
        rows.append({'key': key, 'file': file, 'names': names.get(key, []), 'fail': bool(bad),
                     'level0': res[0] if res[0]['level'] == 0 else None, 'worst': max(res, key=lambda x: x['mae'])})
    a.out.write_text(json.dumps({'low_alpha': LOW, 'mae_max': MAE_MAX, 'stats': stats, 'entries': rows}, indent=1),
                     encoding='utf-8')
    print(stats)
    for x in sorted((x for x in rows if x['fail']), key=lambda x: -x['worst']['mae']):
        print(f"FAIL {x['key'][:2]} {', '.join(n.split(':')[1] for n in x['names'][:2]):40s} worst {x['worst']} L0 {x['level0']}")


if __name__ == '__main__':
    main()

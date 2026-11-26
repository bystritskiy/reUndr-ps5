#!/usr/bin/env python3
"""Real-ESRGAN output -> NHD2 files + merged NHDX manifest (a copy of the base pack plus these).

Per texture (textures.json from extract/prep): colour from ai_out/<key>__rgb.png (or nearest for
flat colour), alpha from ai_out/<key>__a.png (smooth alpha with content), bicubic + threshold
(DXT1 punch-through) or bicubic. DXT1 -> BC1 (punch-through kept), DXT3/DXT5 -> BC3. Own mip chain
down to 4 px on the short side. Record = build-hd-pack.py's: Xbox format/size/bytes/sampled/FNV-1a.
"""
import argparse, hashlib, io, json, re, shutil, struct, sys
from pathlib import Path
import numpy as np
import etcpak
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prep import PAD  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
MANIFEST_LIMIT = 4096  # hd_textures.c
SKIN = re.compile(r'_SKIN\d+$')  # traffic skins live in CARS/TEXTURES.BIN but are a car's own texture


def outside_repo(path):
    """Refuse to write game-derived images inside the repository: they are the user's, never committed."""
    p = Path(path).resolve()
    if p == ROOT or ROOT in p.parents:
        raise SystemExit(f'{path}: must be outside the repository (game-derived data is not committed)')
    return path


def read_manifest(folder):
    data = (folder / 'manifest.bin').read_bytes()
    magic, version, count, size = struct.unpack_from('<4sIII', data)
    if magic != b'NHDX' or version != 1 or size != 84 or len(data) != 16 + count * 84:
        raise SystemExit(f'{folder}: not an NHDX manifest')
    return [data[16 + i * 84:16 + (i + 1) * 84] for i in range(count)]


def ai(path, size, scale):
    img = Image.open(path).convert('RGB')
    img = img.crop((PAD * 4, PAD * 4, img.width - PAD * 4, img.height - PAD * 4))
    if img.size != (size[0] * 4, size[1] * 4):
        raise SystemExit(f'{path}: unexpected AI output size {img.size}')
    return img if scale == 4 else img.resize((size[0] * scale, size[1] * scale), Image.Resampling.LANCZOS)


def resize(im, size, method=Image.Resampling.LANCZOS, straight=False):
    """straight: per channel. Pillow premultiplies RGBA resizes, so colour under alpha~0 is lost - fine for
    DXT cut-outs, wrong for P8, whose alpha is often a mask (road: reflection) over real colour."""
    return Image.merge(im.mode, [c.resize(size, method) for c in im.split()]) if straight else im.resize(size, method)


def encode(rgba, punch):
    """One mip: BC1 (punch-through via Pillow, opaque via etcpak) or BC3 (etcpak)."""
    w, h = rgba.size
    if punch:
        b = io.BytesIO(); rgba.save(b, format='DDS', pixel_format='DXT1')
        return b.getvalue()[128:]
    raw = rgba.tobytes()
    return etcpak.compress_bc3(raw, w, h) if punch is None else etcpak.compress_bc1(raw, w, h)


def build(t, work, scale):
    w, h = t['w'], t['h']
    W, H = w * scale, h * scale
    src = Image.open(work / 'src' / f"{t['key']}.png").convert('RGBA')
    rgb = ai(work / 'ai_out' / f"{t['key']}__rgb.png", (w, h), scale) if t['ai_rgb'] \
        else src.convert('RGB').resize((W, H), Image.Resampling.NEAREST)
    if t['ai_alpha']:
        alpha = ai(work / 'ai_out' / f"{t['key']}__a.png", (w, h), scale).getchannel('R')
    else:
        alpha = src.getchannel('A').resize((W, H), Image.Resampling.BICUBIC)
    # P8: opaque -> BC1; any alpha -> BC3 (its transparent texels have real colour that filtering shows; BC1 would blacken it)
    bc1 = t['format'] == 0x0C or (t['format'] == 0x0B and t['alpha'] == 'none')
    punch = t['format'] == 0x0C and t['alpha'] != 'none'   # True: BC1 1-bit alpha, False: opaque BC1, None: BC3
    if bc1 and not punch:
        alpha = Image.new('L', (W, H), 255)
    payload, levels, lw, lh = b'', 0, W, H
    base = Image.merge('RGBA', (*rgb.split(), alpha))
    while True:
        mip = base if levels == 0 else resize(base, (lw, lh), straight=t['format'] == 0x0B)
        if punch:  # DXT1 semantics: alpha is 0 or 255 at every level
            r, g, b, a = mip.split()
            mip = Image.merge('RGBA', (r, g, b, a.point(lambda v: 255 if v >= 128 else 0)))
        payload += encode(mip, punch if bc1 else None)
        levels += 1
        if min(lw, lh) <= 4:
            break
        lw, lh = lw // 2, lh // 2
    color = 0x0C if bc1 else 0x0F
    body = struct.pack('<4sIIIII', b'NHD2', W, H, len(payload), color, levels) + payload
    return body, W, H, color, levels, base


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('work', type=Path)
    ap.add_argument('--base', type=Path, required=True, help='existing pack to copy and extend (left untouched)')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--scale', type=int, default=4, choices=(1, 2, 4))
    ap.add_argument('--scale-shared', type=int, default=4, choices=(1, 2, 4),
                    help='scale for textures not from the car folder (CARS/TEXTURES.BIN decals)')
    ap.add_argument('--big-above', type=int, default=0, help='source long side above this px uses --scale-big (0: off)')
    ap.add_argument('--scale-big', type=int, default=2, choices=(1, 2, 4))
    ap.add_argument('--label', default='cars')
    ap.add_argument('--replace', action='store_true',
                    help='overwrite base records with the same key (default: keep the base entry)')
    a = ap.parse_args()
    outside_repo(a.out)
    if a.out.resolve() == a.base.resolve():
        raise SystemExit('--out must differ from --base')
    meta = json.loads((a.work / 'textures.json').read_text(encoding='utf-8'))
    a.out.mkdir(parents=True, exist_ok=True)
    base_records = read_manifest(a.base)
    for f in a.base.iterdir():
        if f.is_file() and not (a.out / f.name).exists():
            shutil.copy2(f, a.out / f.name)
    records = {}
    for r in base_records:
        c, sw, sh, sb, sample, _, _, lo, hi = struct.unpack_from('<9I', r)
        records[(c, sw, sh, sb, sample, lo | hi << 32)] = r
    (a.work / 'hd_png').mkdir(exist_ok=True)
    report, added, total = [], 0, 0
    for t in meta['textures']:
        if t['skip']:
            continue
        own = all(not n.startswith('CARS/TEXTURES.BIN:') or SKIN.search(n) for n in t['names'])
        scale = a.scale if own else a.scale_shared
        if a.big_above and max(t['w'], t['h']) > a.big_above:
            scale = min(scale, a.scale_big)
        body, W, H, color, levels, base = build(t, a.work, scale)
        base.save(a.work / 'hd_png' / f"{t['key']}.png")
        fmt, wh, sample, strong = t['key'].split('_')
        w, h = map(int, wh.split('x'))
        sb = t.get('source_bytes') or ((w + 3) // 4) * ((h + 3) // 4) * (8 if t['format'] == 0x0C else 16)
        key = (t['format'], w, h, sb, int(sample, 16), int(strong, 16))
        if key in records and not a.replace:
            report.append({'key': t['key'], 'names': t['names'], 'kept': 'base pack entry'}); continue
        name = hashlib.sha256(body).hexdigest()[:32] + '.dxtc'
        (a.out / name).write_bytes(body)
        s = key[-1]
        records[key] = struct.pack('<9I48s', *key[:5], W, H, s & 0xffffffff, s >> 32, name.encode())
        added += 1; total += len(body)
        report.append({'key': t['key'], 'names': t['names'], 'hd_size': [W, H], 'hd_format': color,
                       'levels': levels, 'bytes': len(body), 'file': name})
    if len(records) > MANIFEST_LIMIT:
        raise SystemExit(f'{len(records)} records > {MANIFEST_LIMIT}')
    ordered = sorted(records.values(), key=lambda r: struct.unpack_from('<I', r, 16)[0])
    (a.out / 'manifest.bin').write_bytes(struct.pack('<4sIII', b'NHDX', 1, len(ordered), 84) + b''.join(ordered))
    (a.out / f'{a.label}.json').write_text(json.dumps({'added': added, 'bytes': total, 'textures': report}, indent=1),
                                          encoding='utf-8')
    disk = sum(f.stat().st_size for f in a.out.iterdir() if f.is_file())
    dxtc = sum(f.stat().st_size for f in a.out.glob('*.dxtc'))
    print(f'added {added} records ({total / 2**20:.1f} MB), base {len(base_records)}, manifest {len(ordered)}; '
          f'pack on disk {disk / 2**20:.1f} MB, NHD2 (preload-resident) {dxtc / 2**20:.1f} MB')


if __name__ == '__main__':
    main()

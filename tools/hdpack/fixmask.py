#!/usr/bin/env python3
"""Rebuild the BC2/BC3 entries of a pack that lose colour under low alpha with colour independent of alpha (copy of the pack -> --out).

Why (seen on a PS5 as black patches on the garage floor, FRONTEND/Road_Segment.bin ROAD*): road/floor textures use
alpha as a reflection mask, so the colour under alpha~0 is drawn. Pillow's RGBA resize premultiplies, and finish.py's mips and
review.py's colour correction / Lanczos used it, so that colour went dark (black asphalt). Here, per entry with its
Xbox original (src/<key>.png) and the AI image finish.py saved (hd_png/<key>.png) in a work dir: candidates AI,
AI + colour back-projection, Lanczos - all per channel (straight), BC3 via etcpak (keeps colour under alpha 0) -
first one that passes review.py's thresholds on straight colour (lights: strict) AND maskcheck.py (every level:
RGB MAE <= 12 where the original alpha < 64) wins; none -> record dropped (the game keeps its own texture).
Entries without both images are kept as they are and listed. Unused files are not copied.
"""
import argparse, hashlib, json, shutil, struct, sys
from pathlib import Path
import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from finish import outside_repo, read_manifest  # noqa: E402
from review import TH, TH_LIGHT, corrected_body, fails, is_light, lanczos_body, metrics, nhd2  # noqa: E402
from maskcheck import MAE_MAX, check  # noqa: E402


def decode_levels(body):
    magic, W, H, n, color, lv = struct.unpack_from('<4sIIIII', body)
    out, at, w, h = [], 24, W, H
    for _ in range(lv):
        size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16
        im = Image.frombytes('RGBA', (max(4, w), max(4, h)), body[at:at + size], 'bcn',
                             (2, 'DXT3') if color == 0x0E else (3, 'DXT5')).crop((0, 0, w, h))
        out.append(im); at += size; w, h = max(1, w // 2), max(1, h // 2)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pack', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--works', nargs='+', type=Path, required=True, help='work dirs, first match wins')
    a = ap.parse_args()
    outside_repo(a.out)
    if a.out.exists():
        raise SystemExit(f'{a.out} exists')
    names = {}
    for j in a.pack.glob('*.json'):
        try:
            for t in json.loads(j.read_text(encoding='utf-8')).get('textures', []):
                names.setdefault(t['key'], t['names'])
        except (ValueError, AttributeError, KeyError, TypeError):
            pass
    a.out.mkdir(parents=True)
    records, rows = read_manifest(a.pack), []
    counts = {}
    for i, r in enumerate(records):
        c, sw, sh, sb, s, W, H, lo, hi = struct.unpack_from('<9I', r)
        key = f"{c:02x}_{sw}x{sh}_{s:08x}_{lo | hi << 32:016x}"
        file = r[36:].split(b'\0')[0].decode()
        body = (a.pack / file).read_bytes()
        color = struct.unpack_from('<I', body, 16)[0] if body[:4] == b'NHD2' else 0
        work = next((w for w in a.works if (w / 'src' / f'{key}.png').exists() and (w / 'hd_png' / f'{key}.png').exists()
                     and Image.open(w / 'hd_png' / f'{key}.png').size == (W, H)), None)
        old = []
        if color in (0x0E, 0x0F) and work is not None:
            old = check(Image.open(work / 'src' / f'{key}.png').convert('RGBA'), decode_levels(body))
        old_mae = max((x['mae'] for x in old if x), default=0.0)
        if color not in (0x0E, 0x0F) or work is None or old_mae <= MAE_MAX:
            kind = 'kept: BC1/NHD1' if color not in (0x0E, 0x0F) else 'kept: no AI source' if work is None else 'kept: colour under low alpha ok'
            counts[kind] = counts.get(kind, 0) + 1
            if color in (0x0E, 0x0F) and work is None:
                rows.append({'key': key, 'names': names.get(key, []), 'result': kind, 'file': file})
            continue
        orig = Image.open(work / 'src' / f'{key}.png').convert('RGBA')
        nm = names.get(key, [f'?:{key}'])
        th = TH_LIGHT if is_light(nm) else TH
        p8 = key.startswith('0b')  # P8 alpha is often a mask: compare colour straight; DXT: as review.py accepted it
        row = {'key': key, 'names': nm, 'work': work.name, 'old_file': file, 'old_lowalpha_mae': old_mae}
        tried = []
        for kind, make in (('ai', lambda: nhd2(Image.open(work / 'hd_png' / f'{key}.png').convert('RGBA'), False, False, True)),
                           ('corrected', lambda: corrected_body(orig, work / 'hd_png' / f'{key}.png', False, False, straight=True)),
                           ('lanczos', lambda: lanczos_body(orig, W, H, False, False, True))):
            if kind != 'ai' and th is TH_LIGHT:
                break  # lights: AI or the original, as in review.py
            new = make()
            lv = decode_levels(new)
            m = metrics(orig, lv[0], straight=p8)
            low = check(orig, lv)
            low_mae = max((x['mae'] for x in low if x), default=0.0)
            f = fails(m, th) + (['lowalpha'] if low_mae > MAE_MAX else [])
            tried.append({'kind': kind, 'fail': f, 'metrics': m, 'lowalpha_mae': low_mae})
            if not f:
                name = hashlib.sha256(new).hexdigest()[:32] + '.dxtc'
                (a.out / name).write_bytes(new)
                records[i] = r[:36] + name.encode().ljust(48, b'\0')
                row.update(result=kind, file=name)
                break
        else:
            row['result'] = 'dropped'
        if 'result' not in row:
            row['result'] = 'dropped'
        row['tried'] = tried
        counts[row['result']] = counts.get(row['result'], 0) + 1
        rows.append(row)
        if len(rows) % 200 == 0:
            print(len(rows), counts, flush=True)
    dropped = {row['key'] for row in rows if row['result'] == 'dropped'}
    keep = []
    for r in records:
        c, sw, sh, sb, s, W, H, lo, hi = struct.unpack_from('<9I', r)
        if f"{c:02x}_{sw}x{sh}_{s:08x}_{lo | hi << 32:016x}" not in dropped:
            keep.append(r)
    used = {r[36:].split(b'\0')[0].decode() for r in keep}
    for f in used:
        if not (a.out / f).exists():
            shutil.copy2(a.pack / f, a.out / f)
    for j in a.pack.glob('*.json'):
        shutil.copy2(j, a.out / j.name)
    (a.out / 'manifest.bin').write_bytes(struct.pack('<4sIII', b'NHDX', 1, len(keep), 84) + b''.join(keep))
    for f in a.out.glob('*.dxtc'):
        if f.name not in used:
            f.unlink()
    (a.out / 'fixmask.json').write_text(json.dumps({'counts': counts, 'entries': rows}, indent=1), encoding='utf-8')
    resident = sum((a.out / f).stat().st_size for f in used if f.endswith('.dxtc'))
    print(f'done {counts}; manifest {len(keep)} (was {len(records)}); NHD2 resident {resident / 2**20:.1f} MB')


if __name__ == '__main__':
    main()

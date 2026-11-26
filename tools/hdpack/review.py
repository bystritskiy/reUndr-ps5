#!/usr/bin/env python3
"""Fidelity review of AI entries in a pack: HD downscaled back to Xbox size vs the Xbox original.

Per entry from the finish.py reports (--report LABEL=WORK): RGB MAE per channel and mean colour shift
(opaque texels), PSNR, alpha coverage diff (a>=128 flips) and alpha MAE, SSIM of luma over grey, and a
detail ratio (HD high-pass energy vs a Lanczos upscale of the original - invented patterns).
Flagged -> AI with back-projection colour correction, then plain Lanczos, each checked again; still failing -> dropped
(game shows its own texture). Rewrites manifest.bin, deletes files no record uses, writes review.json and a
contact sheet of the worst pairs. Run only on a pack folder this pipeline made (it deletes unused files).
"""
import argparse, hashlib, json, re, struct, sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from finish import encode, outside_repo, read_manifest, resize  # noqa: E402
from prep import fill_transparent  # noqa: E402

TH = {'mae': 12.0, 'shift': 5.0, 'psnr': 22.0, 'acov': 0.02, 'amae': 12.0, 'ssim': 0.80, 'detail': 4.0}


def decode(path):
    f = path.read_bytes()
    _, W, H, _, color, _ = struct.unpack_from('<4sIIIII', f)
    n = W * H // 2 if color == 0x0C else W * H
    return Image.frombytes('RGBA', (W, H), f[24:24 + n], 'bcn', (1, 'DXT1') if color == 0x0C else (3, 'DXT5'))


def box(x, k):
    """Mean over a k x k window (edge-padded), via integral image."""
    p = k // 2
    c = np.pad(x, p, mode='edge').cumsum(0).cumsum(1)
    c = np.pad(c, ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def ssim(a, b, k=7):
    ma, mb = box(a, k), box(b, k)
    va, vb, cv = box(a * a, k) - ma ** 2, box(b * b, k) - mb ** 2, box(a * b, k) - ma * mb
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    return float((((2 * ma * mb + c1) * (2 * cv + c2)) / ((ma ** 2 + mb ** 2 + c1) * (va + vb + c2))).mean())


def luma_over_grey(im):
    a = np.asarray(im, np.float64)
    rgb = a[..., :3] * a[..., 3:] / 255 + 128 * (1 - a[..., 3:] / 255)
    return rgb @ [0.299, 0.587, 0.114]


def metrics(orig, hd, straight=False):
    w, h = orig.size
    o = np.asarray(orig, np.float64)
    d = np.asarray(resize(hd, (w, h), Image.Resampling.BOX, straight), np.float64)
    if straight:  # P8: every texel's colour is real (alpha may be a mask), compare colour and alpha apart
        diff = (d[..., :3] - o[..., :3]).reshape(-1, 3)
    else:
        # premultiplied colour (what alpha blending shows): RGB under alpha~0 is junk after any RGBA resize
        m = np.maximum(o[..., 3], d[..., 3]) > 0
        if not m.any():
            m = np.ones(m.shape, bool)
        diff = (d[..., :3] * d[..., 3:] / 255)[m] - (o[..., :3] * o[..., 3:] / 255)[m]
    mse = float((diff ** 2).mean())
    s = hd.width // w
    lan = orig.resize(hd.size, Image.Resampling.LANCZOS)
    hp = lambda x: x - box(x, 2 * s + 1)
    lh, ll = luma_over_grey(hd), luma_over_grey(lan)
    return {'mae': round(float(np.abs(diff).mean(0).max()), 2), 'shift': round(float(np.abs(diff.mean(0)).max()), 2),
            'psnr': round(99.0 if mse == 0 else 10 * np.log10(255 ** 2 / mse), 2),
            'acov': round(float(((o[..., 3] >= 128) != (d[..., 3] >= 128)).mean()), 4),
            'amae': round(float(np.abs(d[..., 3] - o[..., 3]).mean()), 2),
            'ssim': round(ssim(luma_over_grey(orig), luma_over_grey(hd.resize((w, h), Image.Resampling.BOX))), 4),
            'detail': round(float(hp(lh).std() / (hp(ll).std() + 1.0)), 2)}


# light sprites/lamps must keep the original falloff and brightness: stricter limits, failing -> dropped
LIGHT = re.compile(r'FLARE|GLOW|LIGHT|LAMP|HALO|CORONA|LENS|STREAK|SUN|NEON|BULB')
TH_LIGHT = {'mae': 4.0, 'shift': 1.5, 'psnr': 32.0, 'acov': 0.005, 'amae': 3.0, 'ssim': 0.95, 'detail': 2.5}


def is_light(names):
    return any(LIGHT.search(n.split(':')[1]) for n in names)


def fails(m, th=TH):
    return [k for k in th if (m[k] < th[k] if k in ('psnr', 'ssim') else m[k] > th[k])]


def nhd2(big, bc1, punch, straight=False):
    """finish.py's mip chain and encoders for an HD RGBA image."""
    W, H = big.size
    if bc1 and not punch:
        big.putalpha(255)
    payload, levels, lw, lh = b'', 0, W, H
    while True:
        mip = big if levels == 0 else resize(big, (lw, lh), straight=straight)
        if punch:
            r, g, b, al = mip.split()
            mip = Image.merge('RGBA', (r, g, b, al.point(lambda v: 255 if v >= 128 else 0)))
        payload += encode(mip, punch if bc1 else None)
        levels += 1
        if min(lw, lh) <= 4:
            break
        lw, lh = lw // 2, lh // 2
    color = 0x0C if bc1 else 0x0F
    return struct.pack('<4sIIIII', b'NHD2', W, H, len(payload), color, levels) + payload


def filled(orig, bc1, punch):
    a = np.asarray(orig)
    rgb = fill_transparent(a[..., :3], a[..., 3] > 0) if bc1 and punch else a[..., :3]
    return np.dstack([rgb, a[..., 3]]).astype(np.uint8)


def lanczos_body(orig, W, H, bc1, punch, straight=False):
    """Plain Lanczos HD."""
    return nhd2(resize(Image.fromarray(filled(orig, bc1, punch), 'RGBA'), (W, H), straight=straight), bc1, punch, straight)


def corrected_body(orig, ai_png, bc1, punch, rounds=2, straight=False):
    """AI HD with back-projection: add the Lanczos-upscaled residual (original - HD box-downscaled) so the
    HD image box-downscales back to the original colours; AI detail above the source frequency stays."""
    o = filled(orig, bc1, punch).astype(np.float32)
    hd = np.asarray(Image.open(ai_png).convert('RGBA'), np.float32)
    H, W = hd.shape[:2]
    for _ in range(rounds):
        down = np.asarray(resize(Image.fromarray(hd.clip(0, 255).round().astype(np.uint8), 'RGBA'),
                                 orig.size, Image.Resampling.BOX, straight), np.float32)
        res = o - down
        up = np.dstack([np.asarray(Image.fromarray(res[..., c], 'F').resize((W, H), Image.Resampling.LANCZOS))
                        for c in range(4)])
        hd = hd + up
    return nhd2(Image.fromarray(hd.clip(0, 255).round().astype(np.uint8), 'RGBA'), bc1, punch, straight)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pack', type=Path)
    ap.add_argument('--report', action='append', required=True, help='LABEL=WORKDIR (pack/LABEL.json from finish.py)')
    ap.add_argument('--sheet', type=Path, required=True)
    a = ap.parse_args()
    outside_repo(a.pack); outside_repo(a.sheet)
    records = read_manifest(a.pack)
    by_file = {}  # file -> records using it (P8: one image under several index/palette orders = several keys, one file)
    for i, r in enumerate(records):
        by_file.setdefault(r[36:].split(b'\0')[0].decode(), []).append(i)
    todo = {}
    for spec in a.report:
        label, work = spec.split('=')
        for t in json.loads((a.pack / f'{label}.json').read_text(encoding='utf-8'))['textures']:
            if t.get('file') in by_file:
                todo[t['file']] = (t, Path(work))
    out, drop = [], set()
    for name, (t, work) in sorted(todo.items()):
        orig = Image.open(work / 'src' / f"{t['key']}.png").convert('RGBA')
        hd = decode(a.pack / name)
        straight = t['key'].startswith('0b')
        m = metrics(orig, hd, straight)
        light = is_light(t['names'])
        row = {'key': t['key'], 'names': t['names'], 'file': name, 'records': len(by_file[name]), 'ai': m, 'light': light,
               'fail': fails(m, TH_LIGHT if light else TH), 'result': 'passed'}
        if row['fail'] and light:
            drop.update(by_file[name]); row['result'] = 'dropped'
        elif row['fail']:
            idx = by_file[name]
            opaque = np.asarray(orig)[..., 3].min() == 255
            bc1 = t['key'].startswith('0c') or (t['key'].startswith('0b') and opaque)  # finish.py's choice
            punch = t['key'].startswith('0c') and not opaque
            # back-projection fixes colour/intensity only; invented structure (ssim/detail/alpha) -> Lanczos
            # acov too: back-projection corrects alpha the same way (P8 road alpha is per-texel noise, Lanczos smooths it away)
            colour_only = set(row['fail']) <= {'shift', 'mae', 'psnr', 'amae', 'acov'}
            for kind, make in (('corrected',lambda: corrected_body(orig, work / 'hd_png' / f"{t['key']}.png", bc1, punch, straight=straight)),
                               ('fallback', lambda: lanczos_body(orig, hd.width, hd.height, bc1, punch, straight))):
                if kind == 'corrected' and not colour_only:
                    continue
                body = make()
                new = hashlib.sha256(body).hexdigest()[:32] + '.dxtc'
                (a.pack / new).write_bytes(body)
                row[kind] = m2 = metrics(orig, decode(a.pack / new), straight)
                if not fails(m2):
                    for i in idx:
                        records[i] = records[i][:36] + new.encode().ljust(48, b'\0')
                    row['result'] = kind; row['file'] = new
                    break
            else:
                drop.update(idx); row['result'] = 'dropped'
        out.append(row)
    records = [r for i, r in enumerate(records) if i not in drop]
    (a.pack / 'manifest.bin').write_bytes(struct.pack('<4sIII', b'NHDX', 1, len(records), 84) + b''.join(records))
    used = {r[36:].split(b'\0')[0].decode() for r in records}
    counts = {k: sum(r['result'] == k for r in out) for k in ('passed', 'corrected', 'fallback', 'dropped')}
    fail_by = {}
    for r in out:
        for k in r['fail']:
            fail_by[k] = fail_by.get(k, 0) + 1
    (a.pack / 'review.json').write_text(json.dumps({'thresholds': TH, 'counts': counts, 'fail_by_metric': fail_by,
                                                    'entries': out}, indent=1), encoding='utf-8')
    resident = sum((a.pack / f).stat().st_size for f in used if f.endswith('.dxtc'))
    print(f'reviewed {len(out)}: {counts}; failed metric counts {fail_by}; manifest {len(records)}; '
          f'NHD2 resident {resident / 2**20:.1f} MB')
    # worst pairs: most thresholds exceeded, then lowest SSIM; right panel = the AI version
    src = {t['key']: (f, w) for f, (t, w) in todo.items()}
    worst = sorted(out, key=lambda r: (-len(r['fail']), r['ai']['ssim']))[:24]
    cell, cols = 200, 4
    sheet = Image.new('RGB', (cols * (2 * cell + 16), 6 * (cell + 30)), (20, 20, 20))
    d = ImageDraw.Draw(sheet)
    for n, r in enumerate(worst):
        f, work = src[r['key']]
        o = Image.open(work / 'src' / f"{r['key']}.png").convert('RGBA')
        k = cell / max(o.size)
        size = (max(1, round(o.width * k)), max(1, round(o.height * k)))
        x, y = (n % cols) * (2 * cell + 16), (n // cols) * (cell + 30)
        for j, im in enumerate((o.resize(size, Image.Resampling.NEAREST),
                                decode(a.pack / f).resize(size, Image.Resampling.LANCZOS))):
            bg = Image.new('RGBA', size, (70, 70, 70, 255)); bg.alpha_composite(im)
            sheet.paste(bg.convert('RGB'), (x + j * (cell + 4), y + 30))
        m = r['ai']
        d.text((x + 2, y + 2), f"{r['names'][0].split(':')[1][:28]} -> {r['result']}", fill=(255, 230, 0))
        d.text((x + 2, y + 15), f"ssim {m['ssim']} mae {m['mae']} psnr {m['psnr']} acov {m['acov']} det {m['detail']}",
               fill=(200, 200, 200))
    sheet.save(a.sheet)
    print(a.sheet)
    removed = [f for f in a.pack.iterdir() if f.suffix in ('.dxtc', '.rgba') and f.name not in used]
    for f in removed:
        f.unlink()
    print(f'unused files removed: {len(removed)}')


if __name__ == '__main__':
    main()

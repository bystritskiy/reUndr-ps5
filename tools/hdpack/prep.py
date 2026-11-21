#!/usr/bin/env python3
"""Prepare Real-ESRGAN inputs from extract.py output and pick what to skip.

ai_in/<key>__rgb.png: colour, unless flat (then finish.py scales it with nearest);
ai_in/<key>__a.png: alpha as grey, only for smooth alpha with content (decal logos live there);
binary DXT1 alpha is scaled bicubic + threshold in finish.py. Inputs are wrap-padded by PAD px
(AI sees tiling neighbours; finish.py crops); DXT1 punch-through texels get their colour from
opaque neighbours (their black would bleed in).
Skips (textures.json 'skip'): blurred shadows/glows/motion blur/masks by name, nothing to upscale.
"""
import argparse, json, re
from pathlib import Path
import numpy as np
from PIL import Image

PAD = 4
FLAT = 4  # std below this: nothing for the AI to sharpen
SKIP_NAME = re.compile(r'SHADOW|BLUR|_NEON$|MASK')  # _NEON$ = underglow; the Dodge Neon's own textures are NEON_*


def fill_transparent(rgb, opaque):
    rgb, done = rgb.astype(np.float32), opaque.copy()
    for _ in range(64):
        if done.all():
            break
        acc, cnt = np.zeros_like(rgb), np.zeros(done.shape, np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                m = np.roll(np.roll(done, dy, 0), dx, 1)
                acc += np.roll(np.roll(rgb, dy, 0), dx, 1) * m[..., None]
                cnt += m
        new = ~done & (cnt > 0)
        rgb[new] = acc[new] / cnt[new][:, None]
        done |= new
    return rgb.clip(0, 255).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('work', type=Path)
    ap.add_argument('--skip', default=None, help='extra regex on texture name to skip (city: neon/arrows/lookups)')
    a = ap.parse_args()
    extra = re.compile(a.skip) if a.skip else None
    meta = json.loads((a.work / 'textures.json').read_text(encoding='utf-8'))
    (a.work / 'ai_in').mkdir(exist_ok=True)
    for t in meta['textures']:
        img = np.asarray(Image.open(a.work / 'src' / f"{t['key']}.png").convert('RGBA'))
        rgb, alpha = img[..., :3], img[..., 3]
        opaque = alpha > 0
        std = float(rgb[opaque].std()) if opaque.any() else 0.0
        t['rgb_std'] = round(std, 2)
        t['alpha'] = 'none' if alpha.min() == 255 else ('binary' if set(np.unique(alpha)) <= {0, 255} else 'smooth')
        t['ai_rgb'] = std >= FLAT
        t['ai_alpha'] = t['alpha'] == 'smooth' and float(alpha.std()) >= FLAT
        reason = None
        if any(SKIP_NAME.search(n.split(':')[1]) for n in t['names']):
            reason = 'name: shadow/blur/neon/mask (smooth, AI adds nothing)'
        elif extra and any(extra.search(n.split(':')[1]) for n in t['names']):
            reason = 'name: --skip'
        elif not opaque.any():
            reason = 'fully transparent'
        elif not t['ai_rgb'] and not t['ai_alpha']:
            reason = f'flat colour, no alpha detail (std {std:.1f})'
        t['skip'] = reason
        if reason:
            continue
        if t['format'] == 0x0C and t['alpha'] != 'none':
            rgb = fill_transparent(rgb, opaque)
        pad = lambda x: np.pad(x, ((PAD, PAD), (PAD, PAD)) + ((0, 0),) * (x.ndim - 2), mode='wrap')
        if t['ai_rgb']:
            Image.fromarray(pad(rgb)).save(a.work / 'ai_in' / f"{t['key']}__rgb.png")
        if t['ai_alpha']:
            Image.fromarray(pad(alpha)).convert('RGB').save(a.work / 'ai_in' / f"{t['key']}__a.png")
    (a.work / 'textures.json').write_text(json.dumps(meta, indent=1), encoding='utf-8')
    skipped = [t for t in meta['textures'] if t['skip']]
    print(f"{len(meta['textures'])} textures, {len(skipped)} skipped:")
    for t in skipped:
        print(' ', t['skip'], '|', ', '.join(sorted({n.split(':')[1] for n in t['names']})))


if __name__ == '__main__':
    main()

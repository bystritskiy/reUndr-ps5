#!/usr/bin/env python3
"""Extract a car's DXT1/3/5 textures (level 0) to RGBA PNG, one per unique HD key.
--p8: palettised P8 (NV2A 0x0B) textures instead, decoded with their own TPK palette.

Key = what hd_textures.c matches: Xbox format, size, level-0 bytes, nv2a sampled hash, FNV-1a.
P8 (nfsu1_hd_find_p8): sampled hash of the w*h indices, FNV-1a over the indices then the palette,
source bytes = w*h + palette bytes - the same indices under another palette are another key.
Writes <out>/src/<key>.png and <out>/textures.json. Game files are only read.
"""
import argparse, functools, json, re, struct, sys
from pathlib import Path
import numpy as np
from PIL import Image

TOOLS = Path(__file__).resolve().parent.parent      # tools/: nfs_assets.py, build-hd-pack.py
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
from nfs_assets import textures, texture_payload  # noqa: E402
import importlib.util
spec = importlib.util.spec_from_file_location('hdpack', TOOLS / 'build-hd-pack.py')
hdpack = importlib.util.module_from_spec(spec); spec.loader.exec_module(hdpack)
sys.path.insert(0, str(Path(__file__).resolve().parent))
from finish import outside_repo  # noqa: E402


BCN = {0x0C: (1, 'DXT1'), 0x0E: (2, 'DXT3'), 0x0F: (3, 'DXT5')}
P8 = 0x0B


@functools.lru_cache(None)
def unswizzle(w, h):
    """nv2a_vk.c swizzle_index() as a (h, w) table of source offsets."""
    def part(n, other, is_x):
        out = np.zeros(n, np.int64)
        for c in range(n):
            o = bit = 0; m = 1
            while (w - 1) >= m or (h - 1) >= m:
                if (w - 1) >= m:
                    if is_x and c & m: o |= 1 << bit
                    bit += 1
                if (h - 1) >= m:
                    if not is_x and c & m: o |= 1 << bit
                    bit += 1
                m <<= 1
            out[c] = o
        return out
    return part(h, w, False)[:, None] | part(w, h, True)[None, :]


def p8_source(data, row):
    """(indices, palette) as the game has them in memory, or None for a layout we do not know.
    The palette offset is relative to the same blob as the pixels: blob + palette in a plain TPK,
    payload[palette - offset] in a JDLZ-compressed one (car TEXTURES/VINYLS: pixels, pad to 128, palette)."""
    offset, palette, _, palette_size, _ = struct.unpack_from('<5I', data, row['record'] + 48)
    w, h = row['width'], row['height']
    if palette_size not in (128, 256, 512, 1024):
        return None
    if 'compressed_entry' in row:
        payload = texture_payload(data, row)
        if palette < offset + w * h:
            return None
        pal = payload[palette - offset:palette - offset + palette_size]
        return (payload[:w * h], pal) if len(pal) == palette_size else None
    at = row['data_offset']
    return data[at:at + w * h], data[at + palette - offset:at + palette - offset + palette_size]


def p8_image(indices, palette, w, h):
    pal = np.frombuffer(palette, np.uint8).reshape(-1, 4)
    pal = np.vstack([pal, np.repeat(pal[:1], 256 - len(pal), 0)])  # past the end reads entry 0 (decode_indexed)
    bgra = pal[np.frombuffer(indices, np.uint8)[unswizzle(w, h)]]
    return Image.fromarray(np.ascontiguousarray(bgra[..., [2, 1, 0, 3]]), 'RGBA')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--game', type=Path, default=ROOT / 'source/Need For Speed Underground',
                    help='your unpacked disc (default: where tools/prepare.py expects it)')
    ap.add_argument('--files', nargs='+', required=True, help='files under --game')
    ap.add_argument('--include', default='.*', help='regex on texture name (per file: FILE=REGEX allowed)')
    ap.add_argument('--p8', action='store_true', help='take P8 (0x0B) textures instead of DXT')
    ap.add_argument('--out', type=Path, required=True, help='work dir (outside the repository)')
    a = ap.parse_args()
    outside_repo(a.out)
    (a.out / 'src').mkdir(parents=True, exist_ok=True)
    seen, counts = {}, {}
    for spec_ in a.files:
        rel, _, rx = spec_.partition('=')
        rx = re.compile(rx or a.include)
        data = (a.game / rel).read_bytes()
        c = counts.setdefault(rel, {'all': 0, 'dxt': 0, 'p8': 0, 'taken': 0, 'other_formats': {}})
        for row in textures(data):
            c['all'] += 1
            fmt = row['format']
            wanted = fmt == P8 if a.p8 else fmt in BCN
            if not wanted:
                c['other_formats'][hex(fmt)] = c['other_formats'].get(hex(fmt), 0) + 1
                continue
            c['p8' if a.p8 else 'dxt'] += 1
            if not rx.search(row['name']):
                continue
            w, h = row['width'], row['height']
            if a.p8:
                src = p8_source(data, row)
                if src is None:
                    c['p8_unknown_layout'] = c.get('p8_unknown_layout', 0) + 1; continue
                indices, palette = src
                key = f"{fmt:02x}_{w}x{h}_{hdpack.sampled(indices):08x}_{hdpack.fnv(indices + palette):016x}"
                source_bytes = len(indices) + len(palette)
            else:
                level0 = texture_payload(data, row)[:hdpack.level_bytes(fmt, w, h)]
                key = f"{fmt:02x}_{w}x{h}_{hdpack.sampled(level0):08x}_{hdpack.fnv(level0):016x}"
                source_bytes = len(level0)
            names = f"{rel}:{row['name']}"
            if key in seen:
                seen[key]['names'].append(names); continue
            c['taken'] += 1
            img = p8_image(indices, palette, w, h) if a.p8 else Image.frombytes('RGBA', (w, h), level0, 'bcn', BCN[fmt])
            img.save(a.out / 'src' / f'{key}.png')
            seen[key] = {'key': key, 'format': fmt, 'w': w, 'h': h, 'source_bytes': source_bytes, 'names': [names]}
    (a.out / 'textures.json').write_text(json.dumps({'counts': counts, 'textures': list(seen.values())}, indent=1),
                                         encoding='utf-8')
    print(json.dumps(counts, indent=1), f'\nunique textures written: {len(seen)}')


if __name__ == '__main__':
    main()

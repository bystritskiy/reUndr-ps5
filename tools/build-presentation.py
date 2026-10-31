#!/usr/bin/env python3
"""Export the selected reUndr art to icon PNG and PS5 BC7 DX10 backgrounds.

Needs Pillow, etcpak and texture2ddecoder. Uses one city panorama for both
selection and launch backgrounds; does not generate or creatively edit art.
"""
from pathlib import Path
import hashlib
import json
import struct

import etcpak
from PIL import Image, ImageChops, ImageStat
import texture2ddecoder

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / 'ps5/assets/bundle'
OUT = ROOT / 'ps5/sce_sys'
W, H = 3840, 2160


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dds_header():
    # One 2D BC7_UNORM surface, no mip chain, DX10 header, straight alpha.
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000
    pixel_format = struct.pack('<2I4s5I', 32, 4, b'DX10', 0, 0, 0, 0, 0)
    header = struct.pack('<4s7I44x', b'DDS ', 124, flags, H, W, W * H, 1, 1)
    header += pixel_format + struct.pack('<5I', 0x1000, 0, 0, 0, 0)
    return header + struct.pack('<5I', 98, 3, 0, 1, 1)


def main():
    OUT.mkdir(exist_ok=True)
    icon_path = ASSETS / 'reundr-icon-selected-source.png'
    city_path = ASSETS / 'reundr-city-source.png'
    with Image.open(icon_path) as source:
        if source.width != source.height:
            raise SystemExit('Selected icon source must be square')
        icon = source.convert('RGB').resize((512, 512), Image.Resampling.LANCZOS)
        icon.save(OUT / 'icon0.png')
    with Image.open(city_path) as source:
        if abs(source.width / source.height - W / H) > 0.02:
            raise SystemExit('City source must already have a 16:9 composition')
        source_size = source.size
        city = source.convert('RGBA').resize((W, H), Image.Resampling.LANCZOS)
    city.putalpha(255)
    params = etcpak.BC7CompressBlockParams()
    # Color-only BC7 mode 1 has implicit alpha 255. The encoder's default
    # mode 6 can quantize opaque alpha to 254, making the shell backdrop
    # slightly translucent; avoid that while retaining the BC7 DDS profile.
    params.m_mode_mask = 1 << 1
    params.m_uber_level = 2
    blocks = etcpak.compress_bc7(city.tobytes(), W, H, params)
    if len(blocks) != W * H:
        raise SystemExit('Incorrect BC7 surface length')
    payload = dds_header() + blocks
    if len(payload) != 8294548:
        raise SystemExit('Incorrect DDS file length')
    for name in ('pic0.dds', 'pic1.dds'):
        (OUT / name).write_bytes(payload)
    # Keep the existing PNG fallback consistent with the selected DDS artwork.
    city.convert('RGB').resize((1920, 1080), Image.Resampling.LANCZOS).save(OUT / 'pic1.png')

    # Independent decoder: etcpak's own BC7 decompressor is not used because
    # its 0.9.15 implementation has crashed.
    decoded = Image.frombytes('RGBA', (W, H),
        texture2ddecoder.decode_bc7(blocks, W, H), 'raw', 'BGRA')
    if decoded.getchannel('A').getextrema() != (255, 255):
        raise SystemExit('Unexpected transparency in encoded background')
    mean = sum(ImageStat.Stat(ImageChops.difference(
        decoded.convert('RGB'), city.convert('RGB'))).mean) / 3
    if mean > 5:
        raise SystemExit('BC7 decode differs excessively from source')
    preview = ROOT / 'build/presentation'
    preview.mkdir(parents=True, exist_ok=True)
    decoded.convert('RGB').resize((1920, 1080), Image.Resampling.LANCZOS).save(preview / 'city-dds-preview.png')
    for name in ('pic0.dds', 'pic1.dds'):
        data = (OUT / name).read_bytes()
        if data[:148] != dds_header() or data[148:] != blocks:
            raise SystemExit('DDS write verification failed: ' + name)
    with Image.open(OUT / 'icon0.png') as check:
        check.load()
        if check.size != (512, 512) or check.mode != 'RGB':
            raise SystemExit('Icon export validation failed')
    report = {
        'date': '2026-10-29', 'title_id': 'PPSA99025', 'app_name': 'reUndr',
        'console_tested': False, 'console_installed': False,
        'sources': {p.relative_to(ROOT).as_posix(): digest(p) for p in (icon_path, city_path)},
        'background_source_size': list(source_size),
        'background_output_size': [W, H],
        'background_format': 'DX10 DDS, BC7_UNORM (DXGI 98), 2D, 1 surface, 1 mip',
        'independent_bc7_decode': 'texture2ddecoder',
        'bc7_mode': 1, 'decoded_alpha': [255, 255],
        'mean_absolute_rgb_error_255': round(mean, 4),
        'both_backgrounds_use_same_city': True,
        'outputs': {name: {'size': (OUT/name).stat().st_size, 'sha256': digest(OUT/name)}
            for name in ('icon0.png', 'pic0.dds', 'pic1.dds', 'pic1.png')},
        'limitations': ['4K export from 1672x941 imagegen master; not native 4K generation',
            'Local file validation only; current console presentation has not been updated']
    }
    (ROOT / 'docs/presentation-validation.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

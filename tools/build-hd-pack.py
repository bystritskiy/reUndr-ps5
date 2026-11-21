#!/usr/bin/env python3
"""Personal HD texture pack from a TexMod .tpf (NFSU RETextured v2.0 and the like) for NFSU1_HD_DIR.

TexMod names each replacement by the CRC32 of the PC surface's top mip (the
register value, without the final inversion). Where the Xbox disc holds the
same DXT bytes, the Xbox texture's CRC is that name: the PC replacement is then
written in the port's NHD2 format (its BC1/BC2/BC3 blocks and mip chain, as
in the .dds), keyed like every HD texture by format, size, sampled hash and
FNV-1a of the Xbox level 0. No image matching: a retexture pack is new art.

The output is for your own use: keep it outside the repository, never
commit or package it. The .tpf is untrusted data: it is only parsed here.
"""
import argparse
import hashlib
import io
import json
import struct
import sys
import zipfile
import zlib
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nfs_assets import textures, texture_payload  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
TPF_XOR = 0x3FA43FA4
# The ZipCrypto password TexMod uses for every .tpf (see JakobPCoder/TexModTools).
TPF_PASSWORD = bytes([
    0x73, 0x2A, 0x63, 0x7D, 0x5F, 0x0A, 0xA6, 0xBD, 0x7D, 0x65, 0x7E, 0x67, 0x61, 0x2A,
    0x7F, 0x7F, 0x74, 0x61, 0x67, 0x5B, 0x60, 0x70, 0x45, 0x74, 0x5C, 0x22, 0x74, 0x5D,
    0x6E, 0x6A, 0x73, 0x41, 0x77, 0x6E, 0x46, 0x47, 0x77, 0x49, 0x0C, 0x4B, 0x46, 0x6F])
FOURCC = {b'DXT1': 0x0C, b'DXT3': 0x0E, b'DXT5': 0x0F}   # NV2A colour formats
RACE_FILES = ['TRACKS/*.BUN', 'TRACKS/*.BIN', 'CARS/*/TEXTURES.BIN', 'CARS/TEXTURES.BIN', 'GLOBAL/*.BUN']
MAX_ENTRY = 64 << 20
MAX_TOTAL = 1 << 30
MANIFEST_LIMIT = 4096                                    # hd_textures.c


def fnv(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return value


def sampled(data):
    """nv2a_vk.c bytes_hash()."""
    length = len(data)
    value = 2166136261 ^ length
    step = max(4, length // 256) & ~3
    for i in range(0, length - 3, step):
        value = ((value ^ struct.unpack_from('<I', data, i)[0]) * 16777619) & 0xffffffff
    return ((value ^ struct.unpack_from('<I', data, length - 4)[0]) * 16777619) & 0xffffffff


def level_bytes(color, w, h):
    return ((w + 3) // 4) * ((h + 3) // 4) * (8 if color == 0x0C else 16)


def texmod_crc(level0):
    return zlib.crc32(level0) ^ 0xffffffff


def read_tpf(path):
    """{crc: dds bytes}. path is the .tpf or a .zip that holds one."""
    raw = Path(path).read_bytes()
    if raw[:4] == b'PK\x03\x04':
        with zipfile.ZipFile(io.BytesIO(raw)) as outer:
            names = [i for i in outer.infolist() if i.filename.lower().endswith('.tpf')]
            if len(names) != 1 or names[0].file_size > MAX_TOTAL:
                raise SystemExit('expected exactly one .tpf of sane size in the archive')
            raw = outer.read(names[0])
    key = np.frombuffer(struct.pack('<I', TPF_XOR) * (len(raw) // 4 + 1), np.uint8)[:len(raw)]
    plain = (np.frombuffer(raw, np.uint8) ^ key).tobytes()
    out, total = {}, 0
    with zipfile.ZipFile(io.BytesIO(plain)) as z:
        for info in z.infolist():
            name = info.filename.rsplit('/', 1)[-1]
            if not name.lower().endswith('.dds'):
                continue
            stem = name[:-4]
            if '_0x' not in stem.lower():
                raise SystemExit('unexpected .tpf entry name: ' + name)
            total += info.file_size
            if info.file_size > MAX_ENTRY or total > MAX_TOTAL:
                raise SystemExit('.tpf entry too large: ' + name)
            crc = int(stem[stem.lower().rindex('_0x') + 3:], 16)
            out[crc] = z.read(info, pwd=TPF_PASSWORD)
    return out


def parse_dds(data):
    """(color, w, h, levels, payload) for a DXT1/3/5 .dds, else None."""
    if len(data) < 128 or data[:4] != b'DDS ' or struct.unpack_from('<I', data, 4)[0] != 124:
        return None
    h, w = struct.unpack_from('<II', data, 12)
    levels = max(1, struct.unpack_from('<I', data, 28)[0])
    pf_flags, fourcc = struct.unpack_from('<I4s', data, 80)
    color = FOURCC.get(fourcc) if pf_flags & 4 else None
    if not color or not (0 < w <= 4096 and 0 < h <= 4096) or levels > 13:
        return None
    need, lw, lh = 0, w, h
    for _ in range(levels):
        need += level_bytes(color, lw, lh)
        lw, lh = max(1, lw // 2), max(1, lh // 2)
    if len(data) < 128 + need:
        return None
    return color, w, h, levels, data[128:128 + need]


def cap_levels(dds, xbox_w, xbox_h, max_scale):
    """Drop leading mips until the top is at most max_scale x the Xbox size."""
    color, w, h, levels, payload = dds
    skip = 0
    while max_scale and skip + 1 < levels and (w > xbox_w * max_scale or h > xbox_h * max_scale):
        payload = payload[level_bytes(color, w, h):]
        w, h, skip = max(1, w // 2), max(1, h // 2), skip + 1
    return color, w, h, levels - skip, payload


def xbox_textures(game, patterns):
    """{texmod crc: {'signature', 'names'}} for every DXT texture in the files."""
    found = {}
    for pattern in patterns:
        for path in sorted(game.glob(pattern)):
            data = path.read_bytes()
            try:
                rows = list(textures(data))
            except (ValueError, KeyError, struct.error):
                continue
            for row in rows:
                if row['format'] not in (0x0C, 0x0E, 0x0F):
                    continue
                size = level_bytes(row['format'], row['width'], row['height'])
                level0 = texture_payload(data, row)[:size]
                if len(level0) != size:
                    continue
                entry = found.setdefault(texmod_crc(level0), {'level0': level0, 'row': row, 'names': set()})
                entry['names'].add(f"{path.relative_to(game)}:{row['name']}")
    return found


def read_manifest(folder):
    data = (folder / 'manifest.bin').read_bytes()
    magic, version, count, size = struct.unpack_from('<4sIII', data)
    if magic != b'NHDX' or version != 1 or size != 84 or len(data) != 16 + count * 84:
        raise SystemExit(f'{folder}: not an NHDX manifest')
    return [data[16 + i * 84:16 + (i + 1) * 84] for i in range(count)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('tpf', type=Path, help='the .tpf, or the .zip it came in')
    ap.add_argument('--game', type=Path, default=ROOT / 'source/Need For Speed Underground')
    ap.add_argument('--out', type=Path, required=True, help='pack folder (outside the repository)')
    ap.add_argument('--files', nargs='+', default=RACE_FILES, help='game files to match, globs under --game')
    # 4: RETextured v2.0 = 46.5 MB of BC if all 141 were resident at once (full size 83.4, x2 12.0).
    ap.add_argument('--max-scale', type=int, default=4,
                    help='HD side at most N x the Xbox side, by dropping .dds mips (0 = keep the full size)')
    ap.add_argument('--base', type=Path, help='another pack to merge in, e.g. build/hd-own (the port UI art)')
    args = ap.parse_args()
    out = args.out.resolve()
    if out == ROOT or ROOT in out.parents:
        raise SystemExit('--out must be outside the repository: the pack is not ours to commit')
    out.mkdir(parents=True, exist_ok=True)

    pc = read_tpf(args.tpf)
    xbox = xbox_textures(args.game, args.files)
    records, report, unmatched, written = {}, [], [], set()
    for crc, dds_bytes in sorted(pc.items()):
        target = xbox.get(crc)
        dds = parse_dds(dds_bytes)
        if not target or not dds:
            unmatched.append({'crc': f'{crc:08X}', 'reason': 'no Xbox texture with this CRC' if dds else 'not DXT1/3/5'})
            continue
        row, level0 = target['row'], target['level0']
        color, w, h, levels, payload = cap_levels(dds, row['width'], row['height'], args.max_scale)
        body = struct.pack('<4sIIIII', b'NHD2', w, h, len(payload), color, levels) + payload
        filename = hashlib.sha256(body).hexdigest()[:32] + '.dxtc'
        if filename not in written:
            (out / filename).write_bytes(body)
            written.add(filename)
        signature = (row['format'], row['width'], row['height'], len(level0), sampled(level0), fnv(level0))
        strong = signature[-1]
        records[signature] = struct.pack('<9I48s', *signature[:5], w, h, strong & 0xffffffff, strong >> 32,
                                         filename.encode())
        report.append({'crc': f'{crc:08X}', 'xbox': sorted(target['names']), 'xbox_size': [row['width'], row['height']],
                       'xbox_format': row['format'], 'pc_size': [dds[1], dds[2]], 'hd_size': [w, h],
                       'hd_format': color, 'levels': levels, 'bytes': len(body), 'file': filename})
    merged = 0
    if args.base:
        for record in read_manifest(args.base):
            color, w, h, size, sample, _, _, lo, hi = struct.unpack_from('<9I', record)
            key = (color, w, h, size, sample, lo | hi << 32)
            if key in records:
                continue  # the pack's replacement wins
            name = record[36:].split(b'\0')[0].decode()
            (out / name).write_bytes((args.base / name).read_bytes())
            written.add(name)
            records[key] = record
            merged += 1
    if len(records) > MANIFEST_LIMIT:
        raise SystemExit(f'{len(records)} records > {MANIFEST_LIMIT}, the runtime limit')
    ordered = sorted(records.values(), key=lambda r: struct.unpack_from('<I', r, 16)[0])
    (out / 'manifest.bin').write_bytes(struct.pack('<4sIII', b'NHDX', 1, len(ordered), 84) + b''.join(ordered))
    for stale in out.iterdir():
        if stale.suffix in ('.dxtc', '.rgba') and stale.name not in written:
            stale.unlink()
    hd_bytes = sum(r['bytes'] for r in report)
    (out / 'manifest.json').write_text(json.dumps({
        'source': str(args.tpf.name), 'max_scale': args.max_scale, 'pc_textures': len(pc),
        'matched': len(report), 'unmatched': unmatched, 'base_records': merged,
        'hd_bytes': hd_bytes, 'textures': report}, indent=1) + '\n', encoding='utf-8')
    print(f'HD pack: {len(pc)} .tpf textures, {len(report)} matched by CRC32, {len(unmatched)} unmatched, '
          f'{merged} base records, {len(ordered)} manifest records, {hd_bytes / 2**20:.1f} MB of BC data')


if __name__ == '__main__':
    main()

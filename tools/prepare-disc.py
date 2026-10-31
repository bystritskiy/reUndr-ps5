#!/usr/bin/env python3
"""Make the optional HD interface pack from a user's own disc.

reUndr runs from the disc as it is. This adds what is drawn over the disc's own
textures and so cannot ship in the public package: logos, badges and arrows. It
takes an unpacked disc or a disc image and writes <out>/nfsu1/hd. The disc is only
read.

Needs potrace and cairo, and tools/requirements.txt.
"""
import argparse
import hashlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FOLDERS = ['CARS', 'TRACKS', 'FRONTEND', 'GLOBAL', 'SOUND', 'MOVIES', 'LANGUAGES', 'MEMCARD']
SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
PARTITIONS = [0, 0x18300000, 0xFD90000, 0x2080000]


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def unpack_dir(image, base, sector, size, target):
    image.seek(base+sector*SECTOR)
    table = image.read(size)
    pending = [0]
    seen = set()
    while pending:
        at = pending.pop()
        if at in seen or at+14 > len(table):
            continue
        seen.add(at)
        left, right, start, length, attributes, name_size = struct.unpack_from('<HHIIBB', table, at)
        if left == 0xFFFF and right == 0xFFFF:
            continue
        name = table[at+14:at+14+name_size].decode('latin-1')
        if not name or '/' in name or '\\' in name or name in ('.', '..'):
            raise SystemExit('The disc image is damaged')
        pending += [offset*4 for offset in (left, right) if offset and offset != 0xFFFF]
        path = target/name
        if attributes & 0x10:
            path.mkdir()
            if length:
                unpack_dir(image, base, start, length, path)
            continue
        image.seek(base+start*SECTOR)
        with path.open('wb') as out:
            left_to_copy = length
            while left_to_copy:
                block = image.read(min(left_to_copy, 1 << 22))
                if not block:
                    raise SystemExit('The disc image is damaged')
                out.write(block)
                left_to_copy -= len(block)


def unpack_image(path, target):
    with path.open('rb') as image:
        for base in PARTITIONS:
            image.seek(base+0x10000)
            volume = image.read(28)
            if volume[:len(MAGIC)] == MAGIC:
                sector, size = struct.unpack_from('<II', volume, len(MAGIC))
                unpack_dir(image, base, sector, size, target)
                return
    raise SystemExit(f'{path} is not an Xbox disc image')


def build(game, target, with_imagegen):
    expected = re.search(r'"([A-F0-9]{64})"', (ROOT/'ps5/src/game_revision.h').read_text()).group(1)
    if not (game/'default.xbe').is_file():
        raise SystemExit(f'No default.xbe in {game}')
    if digest(game/'default.xbe').upper() != expected:
        raise SystemExit('This is not the supported NTSC-U Xbox release')
    missing = [x for x in FOLDERS if not (game/x).is_dir()]
    if missing:
        raise SystemExit('Missing disc folders: '+', '.join(missing))
    command = [sys.executable, str(ROOT/'tools/build-hd-ui.py'), '--game', str(game), '--out', str(target/'hd'),
               '--overlay', str(target/'no-overlay')]
    subprocess.run(command+([] if with_imagegen else ['--no-imagegen']), check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--game', type=Path, required=True, help='disc image (.iso), or unpacked disc with default.xbe')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--with-imagegen', action='store_true', help='keep the private imagegen illustrations')
    a = p.parse_args()
    try:
        import cairosvg
        import numpy
        import PIL
    except (ImportError, OSError):
        raise SystemExit('Install cairo, then: python3 -m pip install -r tools/requirements.txt')
    if not shutil.which('potrace'):
        raise SystemExit('potrace is missing: https://potrace.sourceforge.net')
    game = a.game.resolve()
    target = a.out.resolve()/'nfsu1'
    if (target/'hd/manifest.bin').exists():
        raise SystemExit(f'{target/"hd"} already exists')
    if game.is_file():
        with tempfile.TemporaryDirectory() as work:
            unpack_image(game, Path(work))
            build(Path(work), target, a.with_imagegen)
    else:
        build(game, target, a.with_imagegen)
    print(f'{target/"hd"}: copy this hd folder next to the game, to /data/nfsu1/hd on the console.')


if __name__ == '__main__':
    main()

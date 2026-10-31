#!/usr/bin/env python3
"""Pack upscaled FMVs as AV1 video for ps5/src/fmv_video.c.

The game keeps playing its own .mad; each decoded frame is recognised by its
texture hash and shown from this pack. ps5/assets/fmv-NAME.hashes holds one line
per movie frame, each with every hash seen for it (tools/fmv-align.py). The game
stretches each frame over the whole 16:9 screen, so the filters below cut a
letterboxed movie down to its 16:9 picture. A 4:3 movie stays stretched, as on the
disc. A pack is a header, sorted (hash, frame) pairs and the frames of an IVF file.
The packs are derived from the game's video, so they stay in build/ and dist/ and
are never published.
"""
import argparse
import json
import struct
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
MOVIES = ROOT/'dist/seedvr2-output/final'
HEADER = struct.Struct('<4s7IQ')
AV1_CRF = 16
AV1_PRESET = 6

STORY = 'crop=1420:532:10:94'
STRETCH_720 = 'scale=1280:720'
# name: (upscaled movie, filter, output size, source texture size)
PACKS = {
    '01_DAY': ('01_DAY_720p', STORY, (1420, 532), (512, 256)),
    '02_FAN_e': ('02_FAN_e_720p', STORY, (1420, 532), (512, 256)),
    '02_FAN_s': ('02_FAN_s_720p', STORY, (1420, 532), (512, 256)),
    '04_GUM_e': ('04_GUM_e_720p', 'crop=1432:580:4:72,pad=1548:580:58:0', (1548, 580), (512, 256)),
    '04_GUM_s': ('04_GUM_s_720p', STORY, (1420, 532), (512, 256)),
    '05_ENG': ('05_ENG_720p', STORY, (1420, 532), (512, 256)),
    '06_SAM': ('06_SAM_720p', STORY, (1420, 532), (512, 256)),
    '07_GOD': ('07_GOD_720p', STORY, (1420, 532), (512, 256)),
    '08_EDD': ('08_EDD_720p', STORY, (1420, 532), (512, 256)),
    '09_GBE': ('09_GBE_720p', STORY, (1420, 532), (512, 256)),
    '11_PNK': ('11_PNK_720p', STORY, (1420, 532), (512, 256)),
    '12_BUD': ('12_BUD_720p', STORY, (1420, 532), (512, 256)),
    '13_KIT': ('13_KIT_720p', STORY, (1420, 532), (512, 256)),
    '14_MUT': ('14_MUT_720p', 'crop=1420:532:10:90', (1420, 532), (512, 256)),
    '15_DIK': ('15_DIK_720p', STORY, (1420, 532), (512, 256)),
    '16_RES': ('16_RES_720p', STORY, (1420, 532), (512, 256)),
    '17_MET': ('17_MET_720p', STORY, (1420, 532), (512, 256)),
    '19_FIN': ('19_FIN_720p', STORY, (1420, 532), (512, 256)),
    'DRIFT': ('DRIFT_720p', STRETCH_720, (1280, 720), (512, 512)),
    'PSA': ('PSA_2160p_43', 'crop=2864:1612:8:278', (2864, 1612), (512, 256)),
    'e3_title': ('e3_title_1440p_43', 'crop=1912:1076:4:182', (1912, 1076), (512, 512)),
    'eng_drag': ('eng_drag_720p', STRETCH_720, (1280, 720), (512, 512)),
    'na_boot': ('na_boot_2160p_43', 'null', (2880, 2160), (512, 256)),
}


def probe(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_frames',
                          '-show_entries', 'stream=nb_read_frames',
                          '-of', 'json', str(path)], check=True, capture_output=True, text=True).stdout
    return int(json.loads(out)['streams'][0]['nb_read_frames'])


def build(name, out):
    movie, video_filter, (width, height), (source_w, source_h) = PACKS[name]
    movie = MOVIES/(movie+'.mkv')
    if width % 2 or height % 2:
        raise SystemExit('4:2:0 needs an even size')
    count = probe(movie)
    lines = [line.split() for line in (ROOT/'ps5/assets'/f'fmv-{name}.hashes').read_text().splitlines()]
    if len(lines) != count:
        raise SystemExit(f'{name}: {len(lines)} hash lines for {count} frames')
    entries = sorted({(int(value, 16), index) for index, hashes in enumerate(lines) for value in hashes})
    out.parent.mkdir(parents=True, exist_ok=True)
    stream = out.with_suffix('.ivf')
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(movie), '-an', '-vf', video_filter,
                    '-pix_fmt', 'yuv420p', '-c:v', 'libsvtav1', '-preset', str(AV1_PRESET),
                    '-crf', str(AV1_CRF), '-g', '150', '-svtav1-params', 'tune=0', '-fps_mode', 'passthrough',
                    str(stream)], check=True, stderr=subprocess.DEVNULL)
    ivf = stream.read_bytes()
    stream.unlink()
    if ivf[:4] != b'DKIF':
        raise SystemExit('ffmpeg did not write IVF')
    body = ivf[int.from_bytes(ivf[6:8], 'little'):]
    at = units = 0
    while at < len(body):
        at += 12+int.from_bytes(body[at:at+4], 'little')
        units += 1
    if units != count or at != len(body):
        raise SystemExit(f'{name}: {units} AV1 units for {count} frames')
    start = HEADER.size+len(entries)*8
    with out.open('wb') as pack:
        pack.write(HEADER.pack(b'NFMV', 2, source_w, source_h, width, height, len(entries),
                               width*height*4, start))
        for item in entries:
            pack.write(struct.pack('<II', *item))
        pack.write(body)
    print(f'{out.name}: {count} frames {width}x{height}, {out.stat().st_size} bytes', flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('names', nargs='*', help='default: every movie')
    p.add_argument('--out', type=Path, default=ROOT/'build/fmv')
    p.add_argument('--force', action='store_true', help='rebuild packs that already exist')
    a = p.parse_args()
    for name in a.names or PACKS:
        out = a.out/(name+'.fmv')
        if a.force or not out.exists():
            build(name, out)


if __name__ == '__main__':
    main()

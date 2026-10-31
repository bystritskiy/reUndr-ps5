#!/usr/bin/env python3
"""Turn raw captures from tools/capture-fmv.py into ps5/assets/fmv-NAME.hashes.

A capture line carries a hash and a few texels of the frame it came from, and
says nothing of which movie frame that was: a run can miss frames, and a repeated
frame is not listed at all. So each line is matched to the movie's frames by those
texels, in order, and lines from other movies fall out. The result has one line
per movie frame with every hash seen for it; a frame identical to a matched one
shares its hashes. Frames still without a hash are reported.
"""
import argparse
import subprocess
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT/'source/Need For Speed Underground/MOVIES'
CAPTURES = ROOT/'build/fmv-capture'
TEXELS = 64
MATCH = 4.0    # mean error per channel accepted as the same frame
SAME = 1.0     # mean error between frames counted as the same picture
BOOT = ('PSA', 'e3_title')  # played from the baseline, the others take the PSA slot
AHEAD = ('logo_eng', 'THX_logo', 'na_boot')  # also played ahead of the PSA slot
E3_RUN = 8     # lines in a row of e3_title that mark the end of a movie


def size_of(mad):
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                          'stream=width,height', '-of', 'csv=p=0', str(mad)],
                         check=True, capture_output=True, text=True).stdout
    return tuple(int(x) for x in out.strip().strip(',').split(','))


def unswizzle(index, width, height):
    x = y = 0
    for bit in range(12):
        if 1 << bit < width:
            x |= (index & 1) << bit
            index >>= 1
        if 1 << bit < height:
            y |= (index & 1) << bit
            index >>= 1
    return x, y


def movie_texels(mad, width, height):
    """The texels a capture line lists, one row per frame, as R G B."""
    points = [unswizzle(i*(width*height//TEXELS), width, height) for i in range(TEXELS)]
    rows = np.array([y for _, y in points])
    columns = np.array([x for x, _ in points])
    process = subprocess.Popen(['ffmpeg', '-v', 'error', '-i', str(mad), '-an', '-f', 'rawvideo',
                                '-pix_fmt', 'rgb24', '-'], stdout=subprocess.PIPE)
    result = []
    while True:
        frame = process.stdout.read(width*height*3)
        if len(frame) < width*height*3:
            break
        result.append(np.frombuffer(frame, np.uint8).reshape(height, width, 3)[rows, columns])
    process.wait()
    # ffmpeg gives full range; the game shows the movie's video range, black at 16.
    return 16+np.array(result, np.float32)*219/255


def read(path):
    return [line.split() for line in path.read_text().splitlines() if line.strip()]


def boot_sequence(base):
    """Line numbers where PSA and e3_title start and e3_title ends in a baseline."""
    known = set((ROOT/'ps5/assets/fmv-PSA.hashes').read_text().split())
    hits = [i for i, line in enumerate(base) if line[0] in known]
    end = hits[-1]+1
    return hits[0], end, next(i for i, line in enumerate(base) if i > end and line[1] == '16')


def movie_lines(capture, name, base):
    """The lines of a capture that belong to this movie, without those around it."""
    psa, e3, stop = boot_sequence(base)
    if name == 'PSA':
        return base[psa:e3]
    if name == 'e3_title':
        return base[e3:stop]
    lines = read(capture)
    start = 0
    if name in AHEAD:
        while start < len(lines) and start < psa and lines[start][0] == base[start][0]:
            start += 1
    else:
        ahead = {line[0] for line in base[:psa]}
        lines = [line for line in lines if line[0] not in ahead]
    following = {line[0] for line in base[e3:stop]}
    for end in range(start, len(lines)):
        if all(line[0] in following for line in lines[end:end+E3_RUN]):
            return lines[start:end]
    return lines[start:]


def texels_of(lines, width, height):
    hashes, texels = [], []
    for fields in lines:
        if len(fields) != 3+TEXELS or (int(fields[1]), int(fields[2])) != (width, height):
            continue
        values = np.array([int(x, 16) for x in fields[3:]], np.uint32)
        hashes.append(fields[0])
        texels.append(np.stack([(values >> 16) & 255, (values >> 8) & 255, values & 255], 1))
    return hashes, np.array(texels, np.float32).reshape(-1, TEXELS, 3)


def match(lines, frames):
    """Best frame for each line, in rising order; -1 for a line of another movie."""
    errors = np.stack([np.abs(frames-line).mean((1, 2)) for line in lines]).tolist()
    count, total = len(lines), len(frames)
    NEXT_FRAME, NEXT_LINE, TAKE = 0, 1, 2
    above = [0.0]*(total+1)
    choices = []
    for i in range(count):
        row, choice = [above[0]+MATCH]+[0.0]*total, [NEXT_LINE]*(total+1)
        for j in range(1, total+1):
            error = errors[i][j-1]
            take = above[j-1]+error if error <= MATCH else float('inf')
            row[j], choice[j] = min((row[j-1], NEXT_FRAME), (above[j]+MATCH, NEXT_LINE), (take, TAKE))
        above = row
        choices.append(choice)
    result = [-1]*count
    i, j = count, total
    while i:
        step = choices[i-1][j]
        if step == NEXT_FRAME:
            j -= 1
        else:
            i -= 1
            if step == TAKE:
                j -= 1
                result[i] = j
    return result


def align(name):
    mad = SOURCE/(name+'.mad')
    width, height = size_of(mad)
    frames = movie_texels(mad, width, height)
    total = len(frames)
    lines = [[] for _ in range(total)]
    path = ROOT/'ps5/assets'/f'fmv-{name}.hashes'
    if path.exists():
        old = path.read_text().splitlines()
        if len(old) == total:
            lines = [x.split() for x in old]
    report = []
    bases = sorted(CAPTURES.glob('baseline.*.txt'))
    for capture in bases if name in BOOT else sorted(CAPTURES.glob(f'{name}.*.txt')):
        base = read(bases[0] if capture not in bases else capture)
        hashes, texels = texels_of(movie_lines(capture, name, base), width, height)
        if not hashes:
            continue
        matched = match(texels, frames)
        chosen = [(i, j) for i, j in enumerate(matched) if j >= 0]
        for i, j in chosen:
            if hashes[i] not in lines[j]:
                lines[j].append(hashes[i])
        worst = max((float(np.abs(frames[j]-texels[i]).mean()) for i, j in chosen), default=0)
        report.append(f'{capture.stem}: {len(chosen)} of {len(hashes)} lines, worst {worst:.1f}')
    covered = [i for i in range(total) if lines[i]]
    missing = []
    for i in range(total):
        if lines[i]:
            continue
        near = min(covered, key=lambda k: abs(k-i), default=None)
        if near is not None and np.abs(frames[i]-frames[near]).mean() <= SAME:
            lines[i] = list(lines[near])
        else:
            missing.append(i)
    path.write_text(''.join(' '.join(line)+'\n' for line in lines))
    shown = ' '.join(str(x) for x in missing[:12])
    print(f'{name}: {total} frames, {len(missing)} without a hash ({shown}); {", ".join(report)}', flush=True)
    return covered != []


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('names', nargs='+')
    a = p.parse_args()
    if not all([align(name) for name in a.names]):
        raise SystemExit(1)


if __name__ == '__main__':
    main()

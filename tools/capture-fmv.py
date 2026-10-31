#!/usr/bin/env python3
"""Capture the frame hashes of FMVs on the console, for tools/build-fmv.py.

The game plays its boot movies by itself, and the others only deep in the game,
so each movie takes a turn in place of PSA.mad and the raw NFSU1_FMV_CAPTURE list
is kept in build/fmv-capture/. The name "baseline" leaves the disc's own boot
sequence in place. Close PPSA99025 first. The console's PSA.mad and nfsu1_env.txt
are put back at the end.
"""
import argparse
import ftplib
import io
import json
import os
import subprocess
import time
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TITLE = 'PPSA99025'
MOVIES = '/data/homebrew/'+TITLE+'/game/MOVIES/'
DATA = '/data/nfsu1/'+TITLE+'/'
ENV = DATA+'nfsu1_env.txt'
CAPTURE = DATA+'fmv-capture.txt'
SOURCE = ROOT/'source/Need For Speed Underground/MOVIES'
OUT = ROOT/'build/fmv-capture'
BOOT_SECONDS = 22  # logo_eng, THX_logo and na_boot, ahead of the PSA slot
FPS = 30000/1001


def connect(a):
    ftp = ftplib.FTP()
    ftp.connect(a.addr, a.port, timeout=60)
    ftp.login()
    ftp.voidcmd('TYPE I')
    return ftp


def fetch(ftp, path):
    data = io.BytesIO()
    ftp.retrbinary('RETR '+path, data.write)
    return data.getvalue()


def replace(ftp, path, source):
    ftp.storbinary('STOR '+path+'.part', source, blocksize=4 << 20)
    try:
        ftp.delete(path)
    except ftplib.error_reply:  # ftpsrv answers DELE with 226
        pass
    ftp.rename(path+'.part', path)


def processes(a):
    query = urllib.parse.urlencode({'addr': a.addr})
    with urllib.request.urlopen(a.engine+'/api/ps5/process/list?'+query, timeout=10) as answer:
        return [p for p in json.load(answer)['processes']
                if p.get('title_id') == TITLE and p.get('name') == 'eboot.bin']


def close_game(a):
    base = f'http://{a.addr}:{a.manager}'
    for p in processes(a):
        request = urllib.request.Request(f"{base}/process_kill?pid={p['pid']}", method='POST')
        request.add_header('X-PLDMGR-Request', '1')
        request.add_header('Origin', base)
        urllib.request.urlopen(request, timeout=10).read()
    for _ in range(40):
        if not processes(a):
            return
        time.sleep(0.5)
    raise SystemExit('PPSA99025 did not close')


def frame_count(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-count_packets', '-select_streams', 'v:0',
                          '-show_entries', 'stream=nb_read_packets', '-of', 'csv=p=0', str(path)],
                         check=True, capture_output=True, text=True).stdout
    return int(out.strip().strip(','))


def capture(a, name, seconds):
    subprocess.run(['python3', '-I', a.launcher, 'launch', '--addr', a.addr, '--title', TITLE], check=True)
    started = time.time()
    while time.time()-started < seconds:
        time.sleep(2)
    close_game(a)
    ftp = connect(a)
    data = fetch(ftp, CAPTURE)
    ftp.quit()
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT/(name+'.txt')).write_bytes(data)
    print(f'{name}: {len(data.splitlines())} lines', flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('movies', nargs='+', help='names without .mad, or baseline')
    p.add_argument('--run', default='1', help='kept in the file name, to repeat a capture')
    p.add_argument('--addr', default=os.environ.get('PS5_ADDR'))
    p.add_argument('--port', type=int, default=int(os.environ.get('PS5_FTP_PORT', '2121')))
    p.add_argument('--manager', type=int, default=int(os.environ.get('PS5_MANAGER_PORT', '8084')))
    p.add_argument('--engine', default=os.environ.get('PS5UPLOAD_ENGINE', 'http://127.0.0.1:19113'))
    p.add_argument('--launcher', required=True)
    p.add_argument('--margin', type=float, default=12.0, help='seconds past the end of the movie')
    a = p.parse_args()
    if processes(a):
        raise SystemExit('PPSA99025 is running; close it first')
    ftp = connect(a)
    env = fetch(ftp, ENV)
    lines = [x for x in env.decode().splitlines() if not x.startswith('NFSU1_FMV_CAPTURE=')]
    replace(ftp, ENV, io.BytesIO('\n'.join(lines+['NFSU1_FMV_CAPTURE='+CAPTURE, '']).encode()))
    ftp.quit()
    try:
        for name in a.movies:
            if name == 'baseline':
                capture(a, f'{name}.{a.run}', 150)
                continue
            mad = SOURCE/(name+'.mad')
            ftp = connect(a)
            with mad.open('rb') as source:
                replace(ftp, MOVIES+'PSA.mad', source)
            ftp.quit()
            capture(a, f'{name}.{a.run}', BOOT_SECONDS+frame_count(mad)/FPS+a.margin)
    finally:
        close_game(a)
        ftp = connect(a)
        replace(ftp, ENV, io.BytesIO(env))
        with (SOURCE/'PSA.mad').open('rb') as source:
            replace(ftp, MOVIES+'PSA.mad', source)
        ftp.quit()


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Ship an HD texture pack as a patch that only your own disc can open.

Every file of the pack is XORed with a SHAKE-256 stream keyed by the original texture it
replaces: the bytes the runtime hashes to find it (hd_textures.c find(): level 0 of a DXT
texture, or the indices then the palette of a P8 one). That key is nowhere in the patch, so
without the disc the patch is noise; with it, `apply` rebuilds the pack byte for byte
(every file is checked against its SHA-256). Files that already ship in the release package
(PPSA99025/hd) are not in the patch: `apply` copies them from --base.

On the console nothing needs to run: copy the patch folder beside the hd folder of the installed game
(/data/reUndr/hdpatch, or reUndr/hdpatch on the USB drive that holds reUndr/game).
On its first start the game opens every file from its own texture files, behind a loading screen
(hd_textures.c nfsu1_hd_patch_prepare, from index.bin), and writes them into reUndr/hd; an hd folder
that came from anything else is kept as hd.bak-<time>.

  make:  python3 tools/hdpatch/hdpatch.py make  --game <disc> --pack <hd> --base <PPSA99025/hd> --out <patch>
  apply: python3 tools/hdpatch/hdpatch.py apply --game <disc> --patch <patch> --base <PPSA99025/hd> --out <hd>

--game is the unpacked disc (the folder with default.xbe, CARS, TRACKS, ...). Game files are
only read; nothing derived from them may be written inside the repository.
"""
import argparse, collections, hashlib, importlib.util, json, os, struct, sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS)); sys.path.insert(0, str(TOOLS / 'hdpack'))
from nfs_assets import textures, texture_payload  # noqa: E402
from extract import p8_source  # noqa: E402
from finish import outside_repo  # noqa: E402
_spec = importlib.util.spec_from_file_location('build_hd_pack', TOOLS / 'build-hd-pack.py')
hdpack = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(hdpack)

DOMAIN = b'reUndr hdpatch 1\0'
P8 = 0x0B


def read_manifest(folder):
    data = (folder / 'manifest.bin').read_bytes()
    magic, ver, count, size = struct.unpack_from('<4sIII', data)
    if magic != b'NHDX' or ver != 1 or size != 84 or len(data) != 16 + 84 * count:
        raise SystemExit(f'{folder}: not an NHDX manifest')
    recs = []
    for i in range(count):
        c, sw, sh, sb, sample, _, _, lo, hi = struct.unpack_from('<9I', data, 16 + 84 * i)
        name = data[16 + 84 * i + 36:16 + 84 * (i + 1)].split(b'\0')[0].decode()
        recs.append(dict(color=c, w=sw, h=sh, bytes=sb, sample=sample, fnv=hi << 32 | lo, file=name))
    return data, recs


def find_sources(game, recs, only=None):
    """record index -> the source bytes the runtime hashes, for every record (of `only`) found on the disc."""
    want = collections.defaultdict(list)
    for i, r in enumerate(recs):
        if only is not None and i not in only:
            continue
        want[(r['color'], r['w'], r['h'])].append(i)
    found = {}
    for path in sorted(Path(d) / f for d, _, fs in os.walk(game, followlinks=True) for f in fs):  # a disc folder may be a link
        data = path.read_bytes()
        try:
            rows = list(textures(data))
        except Exception:  # not a texture container
            continue
        for row in rows:
            idx = want.get((row['format'], row['width'], row['height']))
            if not idx or all(i in found for i in idx):
                continue
            try:
                if row['format'] == P8:
                    s = p8_source(data, row)
                    if s is None:
                        continue
                    src, sampled_over = s[0] + s[1], s[0]
                else:
                    src = texture_payload(data, row); sampled_over = None
            except Exception:
                continue
            for i in idx:
                r = recs[i]
                if i in found or len(src) < r['bytes']:
                    continue
                s = src[:r['bytes']]
                if hdpack.sampled(sampled_over if sampled_over is not None else s) == r['sample'] and hdpack.fnv(s) == r['fnv']:
                    found[i] = s
    return found


def key(source):
    return hashlib.sha256(DOMAIN + source).digest()


def stream(source, name, n):
    return hashlib.shake_256(key(source) + name.encode()).digest(n)


def index_bin(recs, files, sources):
    """index.bin for the runtime, one 72-byte row per manifest record: wrap[32], sha256[32], size, from.
    from 1: the file opens with this record's original (wrap = its key ^ the key the file was made with,
    for a file several records share), 2: copy it from the release's /app0/hd, 0: this record cannot."""
    rows = [struct.pack('<4sIII', b'NHDP', 1, len(recs), 72)]
    for i, r in enumerate(recs):
        v = files[r['file']]
        if v['from'] == 'base':
            frm, wrap = 2, bytes(32)
        elif i in sources:
            frm, wrap = 1, xor(key(sources[v['record']]), key(sources[i]))
        else:
            frm, wrap = 0, bytes(32)
        rows.append(wrap + bytes.fromhex(v['sha256']) + struct.pack('<II', v['size'], frm))
    return b''.join(rows)


def xor(data, pad):
    return (int.from_bytes(data, 'little') ^ int.from_bytes(pad, 'little')).to_bytes(len(data), 'little')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def make(a):
    outside_repo(a.out)
    a.out.mkdir(parents=True, exist_ok=True)
    manifest, recs = read_manifest(a.pack)
    sources = find_sources(a.game, recs)
    files, missing = {}, []
    for i, r in enumerate(recs):
        f = r['file']
        if f in files:
            continue
        data = (a.pack / f).read_bytes()
        if a.base and (a.base / f).is_file() and (a.base / f).read_bytes() == data:
            files[f] = {'from': 'base', 'size': len(data), 'sha256': sha(data)}
            continue
        # the first record of this file that the disc has is its key
        key = next((j for j, q in enumerate(recs) if q['file'] == f and j in sources), None)
        if key is None:
            missing.append(f); continue
        files[f] = {'from': 'disc', 'record': key, 'size': len(data), 'sha256': sha(data)}
        (a.out / 'files').mkdir(parents=True, exist_ok=True)
        (a.out / 'files' / f).write_bytes(xor(data, stream(sources[key], f, len(data))))
    if missing:
        raise SystemExit(f'{len(missing)} files have no original on this disc and are not in --base, e.g. {missing[:5]}')
    (a.out / 'manifest.bin').write_bytes(manifest)
    (a.out / 'index.bin').write_bytes(index_bin(recs, files, sources))
    (a.out / 'patch.json').write_text(json.dumps({'format': 'reUndr hdpatch 1', 'manifest_sha256': sha(manifest),
                                                  'files': files}, indent=1, sort_keys=True), encoding='utf-8')
    n = collections.Counter(v['from'] for v in files.values())
    print(f"patch: {len(recs)} records, {len(files)} files: {n['disc']} from the disc, {n['base']} from --base -> {a.out}")


def apply(a):
    outside_repo(a.out)
    if (a.out / 'manifest.bin').exists():                 # as prepare-disc.py: never write over a pack
        raise SystemExit(f'{a.out} already holds a pack; move it aside first')
    meta = json.loads((a.patch / 'patch.json').read_text(encoding='utf-8'))
    if meta.get('format') != 'reUndr hdpatch 1':
        raise SystemExit(f'{a.patch}: not a reUndr hdpatch 1 patch')
    manifest = (a.patch / 'manifest.bin').read_bytes()
    if sha(manifest) != meta['manifest_sha256']:
        raise SystemExit('manifest.bin does not match patch.json')
    _, recs = read_manifest(a.patch)
    sources = find_sources(a.game, recs, {v['record'] for v in meta['files'].values() if v['from'] == 'disc'})
    a.out.mkdir(parents=True, exist_ok=True)
    bad = []
    for f, v in sorted(meta['files'].items()):
        if v['from'] == 'base':
            data = (a.base / f).read_bytes() if a.base and (a.base / f).is_file() else b''
        elif v['record'] in sources:
            enc = (a.patch / 'files' / f).read_bytes()
            data = xor(enc, stream(sources[v['record']], f, len(enc)))
        else:
            data = b''
        if sha(data) != v['sha256']:
            bad.append(f); continue
        (a.out / f).write_bytes(data)
    if bad:
        raise SystemExit(f'{len(bad)} of {len(meta["files"])} files could not be rebuilt (another disc, a modified file, '
                         f'or --base missing), e.g. {bad[:5]}; {a.out} is incomplete')
    (a.out / 'manifest.bin').write_bytes(manifest)
    print(f'{len(meta["files"])} files rebuilt and checked -> {a.out}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('make', help='pack + disc -> patch')
    m.add_argument('--pack', type=Path, required=True, help='the pack folder (manifest.bin + files)')
    m.add_argument('--out', type=Path, required=True, help='patch folder (outside the repository)')
    p = sub.add_parser('apply', help='patch + disc -> pack')
    p.add_argument('--patch', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True, help='the hd folder to write (outside the repository)')
    for s in (m, p):
        s.add_argument('--game', type=Path, required=True, help='your unpacked disc')
        s.add_argument('--base', type=Path, help='PPSA99025/hd of the release package: its files are not in the patch')
    a = ap.parse_args()
    make(a) if a.cmd == 'make' else apply(a)


if __name__ == '__main__':
    main()

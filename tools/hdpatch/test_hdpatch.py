#!/usr/bin/env python3
"""hdpatch round trip without a disc: the disc lookup is replaced by a fixed source.
make -> apply gives the pack back byte for byte; another source (another disc) rebuilds nothing.
python3 tools/hdpatch/test_hdpatch.py"""
import argparse, os, struct, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hdpatch  # noqa: E402

with tempfile.TemporaryDirectory() as t:
    t = Path(t)
    pack, base, patch, out = t / 'pack', t / 'base', t / 'patch', t / 'out'
    pack.mkdir(); base.mkdir()
    names = [b'a' * 32 + b'.dxtc', b'b' * 32 + b'.rgba']
    recs = b''.join(struct.pack('<9I48s', 0x0C, 4, 4, 8, i, 8, 8, 0, 0, n) for i, n in enumerate(names))
    (pack / 'manifest.bin').write_bytes(struct.pack('<4sIII', b'NHDX', 1, 2, 84) + recs)
    hd = os.urandom(5000)
    (pack / names[0].decode()).write_bytes(hd)
    (pack / names[1].decode()).write_bytes(b'shipped with the release')
    (base / names[1].decode()).write_bytes(b'shipped with the release')
    disc = {0: b'original'}
    hdpatch.find_sources = lambda game, recs, only=None: disc
    ns = argparse.Namespace(game=t, base=base)
    hdpatch.make(argparse.Namespace(**vars(ns), pack=pack, out=patch))
    enc = (patch / 'files' / names[0].decode()).read_bytes()
    assert enc != hd and len(enc) == len(hd) and not (patch / 'files' / names[1].decode()).exists()
    hdpatch.apply(argparse.Namespace(**vars(ns), patch=patch, out=out))
    assert all((out / n.decode()).read_bytes() == (pack / n.decode()).read_bytes() for n in names)
    assert (out / 'manifest.bin').read_bytes() == (pack / 'manifest.bin').read_bytes()
    disc[0] = b'0riginal'                      # another disc: one byte off
    try:
        hdpatch.apply(argparse.Namespace(**vars(ns), patch=patch, out=t / 'out2'))
        raise AssertionError('another disc rebuilt the pack')
    except SystemExit as e:
        assert '1 of 2 files could not be rebuilt' in str(e) and not (t / 'out2' / 'manifest.bin').exists()
print('hdpatch: round trip byte for byte; another disc rebuilds nothing')

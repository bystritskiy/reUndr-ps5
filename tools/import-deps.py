#!/usr/bin/env python3
"""Copy an existing pinned PS5 dependency cache into this project."""
import argparse,shutil,json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser(description=__doc__);p.add_argument('cache',type=Path);a=p.parse_args();cache=a.cache.resolve()
lock=json.loads((root/'ps5/dependencies.json').read_text())
# Validate before replacing only this project's bootstrap symlinks.
for name,(_,sha) in lock['sources'].items():
 actual=subprocess.check_output(['git','-C',str(cache/'build/deps'/name),'rev-parse','HEAD'],text=True).strip()
 if actual!=sha:raise SystemExit(f'{name}: pin mismatch {actual}')
deps=root/'build/deps'
if deps.is_symlink():deps.unlink()
deps.mkdir(exist_ok=True)
for name in [*lock['sources'],'compiler-rt']:
 dest=deps/name
 if dest.exists():continue
 def ignore(src,names):
  if name=='PS5_Vulkan' and Path(src).name=='.deps':return [n for n in names if n!='native']
  return []
 shutil.copytree(cache/'build/deps'/name,dest,symlinks=True,ignore=ignore)
 print('Copied',name,flush=True)
for name,src in [('sdl3',cache/'build/ps5/sdl3'),('shader-compiler',cache/'build/ps5/game-xbox-retail/shader-compiler')]:
 dest=root/'build'/name
 if dest.is_symlink():dest.unlink()
 if not dest.exists():
  # Only the libraries are reused; CMake build paths from the cache are discarded.
  dest.mkdir()
  for file in src.rglob('*.a'):
   target=dest/file.relative_to(src);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(file,target)
 print('Copied PS5 static libraries:',name,flush=True)

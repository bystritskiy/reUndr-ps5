# Making an HD texture pack from your own disc

This is the workflow behind the HD pack described in [GRAPHICS.md](GRAPHICS.md): the game's own
textures, upscaled by an AI model, reviewed against the originals and block-compressed. It is optional
and personal.

- **Nothing here ships textures.** The repository holds only the tools. Every pack is made by each user,
  on their own computer, from their own disc, and stays with them. The tools refuse to write their
  output inside the repository.
- The game itself is still supplied by you, as for the port.

## What you need

- Your own NTSC-U disc's files, unpacked (the folder with `default.xbe`, `CARS`, `TRACKS`, ...; by
  default `source/Need For Speed Underground`, as for building the port, or any folder via `--game`).
- Python 3 with the port's packages: `numpy`, `pillow>=13` (writes the BC1 punch-through mips),
  `etcpak==0.9.15` (see the developer section of the README).
- [Real-ESRGAN ncnn Vulkan](https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0)
  (`realesrgan-ncnn-vulkan`, model `realesrgan-x4plus`, BSD-3-Clause, by xinntao). Any Vulkan GPU; on a
  desktop GPU ~650 textures take about 20 s.
- A **base pack**: the pack the console would otherwise load, so its art stays in. The game reads one pack
  only (`NFSU1_HD_DIR`, by default `hd` beside the game folder, else `/app0/hd`). Use the `hd` folder
  made by `tools/prepare-disc.py` if you made one, otherwise the `hd` folder of the release package
  (`PPSA99025/hd`).

All work folders and packs must be outside the repository. Below, `W` is a work folder per batch and
`PACK_n` a pack folder; each step writes a new pack and leaves its `--base` untouched.

## The steps (one batch)

The scripts are in `tools/hdpack/`; each has `--help`.

1. **Extract** the textures you want, at their Xbox size, one PNG per unique HD key (the same key the
   runtime matches: format, size, sampled hash and FNV-1a of the bytes; for P8, of the indices plus the
   palette):
   ```sh
   python3 tools/hdpack/extract.py --files 'CARS/LANCER/TEXTURES.BIN' --out W
   python3 tools/hdpack/extract.py --files 'TRACKS/STREAML1RA.BUN' --p8 --out W   # P8 textures instead of DXT
   ```
   `--files` takes paths under `--game`, optionally `FILE=REGEX` to pick texture names.
2. **Prepare** the model inputs: colour and (for smooth alpha with content) alpha as separate images,
   wrap-padded by 4 pixels so tiling textures upscale seamlessly. Skips shadows, blurs, underglow and
   masks by name, flat colours, fully transparent textures, and anything matching `--skip`:
   ```sh
   python3 tools/hdpack/prep.py W --skip 'NEON|ARROW|CHEVRON|ROADSTRIP|GUIDE|ENVMAP|GLOW|NOISE|SPLINE'   # the city batch's list
   ```
3. **Upscale** 4x:
   ```sh
   realesrgan-ncnn-vulkan -i W/ai_in -o W/ai_out -n realesrgan-x4plus -s 4 -f png
   ```
4. **Encode** into a new pack: crops the padding, rebuilds alpha (binary DXT1 alpha by bicubic +
   threshold), picks BC1 (opaque or 1-bit alpha) or BC3, builds the mip chain down to 4 px, writes NHD2
   files and a merged `manifest.bin`:
   ```sh
   python3 tools/hdpack/finish.py W --base PACK_0 --out PACK_1 --label cars
   # city-size textures: 4x up to 128 px, 2x above, so the pack fits in memory
   python3 tools/hdpack/finish.py W --base PACK_1 --out PACK_2 --label city --big-above 128 --scale-big 2
   ```
   `--replace` lets the batch overwrite base entries with the same key.
5. **Review** the batch against the originals (in place, on the new pack): each HD image is
   box-downscaled to the Xbox size and compared for colour, alpha, structure (SSIM) and invented
   detail. Colour-only failures are corrected by back-projection, structure or alpha failures fall back to
   a plain Lanczos upscale, and what still fails is dropped (the game keeps its own texture). Lights are
   held to strict limits and dropped on any failure. Writes `review.json` and a contact sheet of the worst
   24 pairs to look at:
   ```sh
   python3 tools/hdpack/review.py PACK_1 --report cars=W --sheet cars-review.png
   ```

## After the last batch

6. **Check the colour under mask alpha** on every mip. Roads and floors use alpha as a reflection mask,
   so the colour under alpha ~0 is drawn; a resize that premultiplies alpha turns it black (seen on the
   console as black patches). Then rebuild or drop the entries that fail, into a new pack:
   ```sh
   python3 tools/hdpack/maskcheck.py PACK_N --works W1 W2 ... --out maskcheck.json
   python3 tools/hdpack/fixmask.py PACK_N --works W1 W2 ... --out PACK_FINAL
   ```
   Run this step: on a 14-texture test batch it still found one decal whose small mips darkened.
7. **Validate** the pack the way the runtime will read it:
   ```sh
   python3 tools/hdpack/check_pack.py PACK_FINAL
   ```
   The runtime accepts at most 4096 entries and 4096x4096 per image.

## Install

Copy the contents of `PACK_FINAL` (`manifest.bin` and the `.dxtc`/`.rgba` files; the `.json` reports are
not needed) to `/data/reUndr/hd` on the console, or `reUndr/hd` on the USB drive that holds the game. In
`/data/reUndr/nfsu1_env.txt`:

```
NFSU1_HD_PRELOAD=1
NFSU1_HD_P8=1
```

`NFSU1_HD_P8=1` only if the pack has P8 entries. `NFSU1_HD_DIR` only if the pack is elsewhere. The log
shows `[HD] Loaded N texture fingerprints` and, with preload, `[HD] Preloaded N files, M MB`. To go back
to the original textures, set `NFSU1_HD=0` or remove the folder.

## The pack used on the test console

Batches, in this order (each with the steps above): car textures of the 19 player cars and 15 traffic
models (decals at 2x, the rest 4x; reviewed by eye on before/after crops, before `review.py` existed),
then the city streams, loading screens and front-end (`--big-above 128 --scale-big 2`), then the city's P8
textures (4x, `--p8`). 3546 entries, ~533 MB of BC data with preload. Car skins, vinyls and wheels are
not in it: the game composites them at run time, so no file on the disc holds their final pixels.

`tools/build-hd-pack.py` (also in this PR) converts a TexMod `.tpf` retexture you own into the same
format, keyed by exact Xbox bytes. It was used for a first test with a PC retexture pack, which was then
replaced entirely by the workflow above, because that pack's art differs from the original.

## Sharing a pack: a patch that needs the disc

A finished pack is made of the disc's own textures, so it is not shared as it is. `tools/hdpatch/hdpatch.py`
turns it into a patch that is noise without the disc: each file is XORed with a SHAKE-256 stream keyed by
the SHA-256 of the original texture it replaces (the bytes the runtime hashes to find it). That key is not
in the patch; whoever has the disc derives it again, and `apply` rebuilds the pack byte for byte, checking
every file against its SHA-256. Files that ship in the release package (`PPSA99025/hd`) are not in the
patch; `apply` takes them from `--base`.

```sh
# the pack's author
python3 tools/hdpatch/hdpatch.py make  --game <disc> --pack <hd> --base <PPSA99025/hd> --out <patch>
# everyone else: their own disc + the patch -> the same hd folder
python3 tools/hdpatch/hdpatch.py apply --game <disc> --patch <patch> --base <PPSA99025/hd> --out <hd>
```

Both read every texture container on the disc (a few minutes). `apply` writes `manifest.bin` only when
every file was rebuilt, so another disc revision, a missing folder or a modified file leaves no half pack
for the game to load; it lists what failed. It refuses an `--out` that already holds a pack. `python3 tools/hdpatch/test_hdpatch.py` checks the round trip.
This does not change what the pack is derived from; it only means the patch alone carries no texture.

On the console nothing has to run on a computer: copy the patch folder beside the game's `hd` folder
(`/data/reUndr/hdpatch`, or `reUndr/hdpatch` on the USB drive). On the first start with a new patch the game
moves an `hd` folder that holds a pack (it has `manifest.bin` or an earlier patch's `hdpatch.id`) to
`hd.bak-<time>`, never deleting it, and writes the patch's manifest into a new `hd`. Each file is then opened
the first time its texture shows, checked against its SHA-256 and written into `hd`; the log says
`[HD] patch: opened <file> <KB> <ms>`. From then on it is an ordinary pack, and preload reads it.

The first start costs time: opening runs on the render thread. Measured on a PS5 (27 Nov 2026, the same
`hd_textures.c` patch code on a 01.001.000 build, a 590 MB patch of 3569 records): the first race opened 668
files in 17.6 s in all, a 16 MB `.rgba` in 1.1 s, which showed as hitches of up to 5 s while the race loaded
and 0.3-1.6 s of opening per 10 s in the race. The second start preloaded 681 files and opened 45 that the
first race had not shown (0.9 s), 41 of them while the race loaded, so that load still hitched for 0.9 s; in
the race no hitch was above 80 ms. No file failed its SHA-256 check, and 11 files read back at random matched
the pack byte for byte. The cost ends once every texture has been shown once.

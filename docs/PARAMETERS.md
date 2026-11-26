# Switches added or used by the performance and graphics changes

All of these are proposals and opt-in, except `RECOMP_FAST_TIME`, which this PR proposes to turn on by
default. With nothing set, every other switch keeps the original behaviour. Set them as `KEY=VALUE`
lines in `/data/reUndr/nfsu1_env.txt`; they are read once at start. Values outside the listed
range fall back to the default unless the row says otherwise.

Background: [PERFORMANCE.md](PERFORMANCE.md) (why the clock and TLS changes), [GRAPHICS.md](GRAPHICS.md)
(what the picture options do), [HD_PACK_WORKFLOW.md](HD_PACK_WORKFLOW.md) (making an HD pack).

## Timing

| Switch | Default | Values | What it does | Cost | When to change |
|---|---|---|---|---|---|
| `RECOMP_FAST_TIME` | **1 (proposed on)** | 0, 1 | Guest clocks (`QueryPerformanceCounter`, `GetTickCount64`, `GetSystemTimeAsFileTime`, so every guest time source) from the TSC, re-anchored to `CLOCK_MONOTONIC` every 2 ms, never going backwards. 0 = the original `clock_gettime` code. Boot line `[time] fast on/off`; when on, a self-check line every 10 s. | Saves ~1 ms of guest time per race frame on the PS5 (a `clock_gettime` system call is ~20 us). Error vs `clock_gettime` 2.7-4.5 us on the console. | Set 0 if anything time-related misbehaves, for example after rest mode (re-anchoring after a TSC jump is tested on a Linux host only). |

## Light trails and glare (NFSU1 only)

The game draws light halos into a trail buffer that is decayed every frame, tuned for 30 fps. These
switches only act on that buffer, recognised by its decay pass; other titles are not affected unless
they draw that exact pass.

| Switch | Default | Values | What it does | Cost | When to change |
|---|---|---|---|---|---|
| `RECOMP_FLARE_FIX` | 0 (off) | 0, 1, 2, 3 | 1: the trail is updated every frame and its decay follows the real frame time, so trails last as long at 60 fps as on the Xbox at 30 fps, and the halo sits on the lamp. 3: the first version (trail updated at 30 Hz, for A/B; flickers with a short trail). 2: every second frame (slow hosts). | One register-copy per frame on the CPU; no extra GPU pass. | Turn on to stop oncoming headlights from smearing into large bright blobs at 60 fps. |
| `RECOMP_FLARE_TRAIL` | 0.10 | 0.05-4.0 | With `RECOMP_FLARE_FIX` on: trail length relative to the Xbox's (1 = the Xbox's length). | none | Raise towards 1 for the original long trails. |
| `RECOMP_FLARE_TRAIL_OPACITY` | 0.7 | 0-1 | With `RECOMP_FLARE_FIX` on: extra dimming of every older copy (1 = none). | none | Raise for more visible trails. |
| `RECOMP_FLARE_GLARE` | 0 (off) | 0, 1, 2 | 1: a soft warm glare (four short streaks and a small halo) around the brightest lights, from the trail buffer only, added onto the finished frame. 2: even frames only (A/B). | One full-screen pass at flip; not measured on its own (everything in GRAPHICS.md together: 3.2-5.2 ms GPU per frame at 4K). | Turn on if oncoming lights should dazzle a little. Independent of `RECOMP_FLARE_FIX`. |
| `RECOMP_FLARE_GLARE_THRESHOLD` | 0.2 | 0-1000 (below 0 or from 1000: default) | Trail brightness where glare starts (smoothstep from here to 1). | none | Lower (the test console used 0.08) for more lights to glare. |
| `RECOMP_FLARE_GLARE_STRENGTH` | 0.5 | same | Streak brightness; 0 = no streaks. | none | Taste. |
| `RECOMP_FLARE_GLARE_LENGTH` | 20 | same | Streak length in the game's own 640x480 pixels (the look does not change with the render scale). | none | Taste. |
| `RECOMP_FLARE_GLARE_HALO` | 0.3 | same | Halo brightness. | none | Taste. |
| `RECOMP_FLARE_GLARE_RADIUS` | 5 | same | Halo radius in the game's pixels. | none | Taste. |

## Image quality

| Switch | Default | Values | What it does | Cost | When to change |
|---|---|---|---|---|---|
| `RECOMP_VK_CUBE_UP` | 1 (off) | 1-4 (clamped) | The six faces of the car reflection cube map are rendered at m times the render scale instead of 1x: sharper reflections on the car bodies. The next frame after a cube is first seen rebuilds its faces. Boot line `[VK] reflection cube faces at ...`. | 1.5: ~+1 ms GPU at 4K (estimate); 2: ~+2.4 ms (estimate). Memory at 2: faces 42 MB + cube 75 MB. | 1.5 is what the test console used. |
| `RECOMP_VK_SSAA` | 1 (off) | 1-2 (clamped) | Every surface is rendered at k times the render scale and the present pass filters it down (4 bilinear taps; below 2:1 with FXAA on, FXAA per tap). | Estimated from pixel count at 4K: 1.5 -> ~7.4 ms GPU (peak ~10.4), 2 -> ~13 ms (peak ~18, over budget). Memory at 4K and 1.5: ~170 MB for the main colour + depth. | **Never run on a PS5**, only on a Linux host. Try 1.33-1.5 at 4K if the GPU has room; not needed for the look described in GRAPHICS.md. |
| `RECOMP_VK_LOD_BIAS` | -0.25 (existing) | any float | Mip bias of mipmapped textures; lower is sharper. | none | The test console used 0, which shimmered less on fine textures. |

## HD texture packs

| Switch | Default | Values | What it does | Cost | When to change |
|---|---|---|---|---|---|
| `NFSU1_HD_DIR` | `hd` beside the game folder (existing), e.g. `/data/reUndr/hd`; else `/app0/hd` | a folder | Where `manifest.bin` and the replacement files are read from. Only one pack is loaded. | - | Only if the pack is somewhere else. |
| `NFSU1_HD_PRELOAD` | 0 (off) | 0, 1 | Reads every NHD2 (`.dxtc`) file of the pack into RAM on a background thread at start, so a texture first seen in a race costs a memory copy instead of a disk read inside the frame. Log: `[HD] Preloaded N files, M MB`. | RAM equal to the pack's `.dxtc` files (533 MB for the full pack described in HD_PACK_WORKFLOW.md). | Turn on with any large pack. |
| `NFSU1_HD_P8` | 0 (off) | 0, 1 | Also looks up palettised P8 (0x0B) textures in the pack, keyed by the index bytes plus the palette in use; a changed palette (paint, blinking) never matches, so the game's own texture is used. | One hash of the indices and palette when a P8 texture's cache entry changes (cost on the console not measured). | Turn on when the pack has P8 entries (road surface, foliage, many props). |
| `NFSU1_HD_DUMP` | unset (existing) | a folder | Writes each texture of 128x128 or more that has no replacement once, to see what a screen is made of. With `NFSU1_HD_P8=1` P8 textures are dumped too. | Disk writes while on. | Only for making packs. |
| `NFSU1_HD` | 1 (existing) | 0, 1 | 0 turns all HD replacements off. | - | A/B against the original textures. |

## Configuration used on the test console

```
RECOMP_FLARE_FIX=1
RECOMP_FLARE_GLARE=1
RECOMP_FLARE_GLARE_THRESHOLD=0.08
RECOMP_VK_CUBE_UP=1.5
RECOMP_VK_LOD_BIAS=0
NFSU1_HD_DIR=/data/reUndr/hd
NFSU1_HD_PRELOAD=1
NFSU1_HD_P8=1
```

`RECOMP_FAST_TIME` is on by default with this PR; `RECOMP_VK_SSAA` was left off. Render scale 4.5
(4K) from the game's Port Settings.

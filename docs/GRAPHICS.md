# Graphics: what changes and why it stays the original game

These are proposals, all behind switches that are off by default ([PARAMETERS.md](PARAMETERS.md)). The
aim was the original Xbox look at 4K and 60 fps, only cleaner. Nothing in the game's content, lighting,
camera, physics or HUD was changed, and no new art was added. The game itself is still supplied by each
user from their own disc.

## What stays as it is

- **Resolution and render scale are the port's own** (Port Settings, `RECOMP_GL_SCALE`). Nothing here
  raises the resolution behind the user's back; supersampling exists as an option but is off and was
  not used.
- Models, level geometry, shaders, lighting, the HUD, FXAA and anisotropic filtering are untouched.
- Textures that an HD pack does not cover, or that fail its review, are the game's own.

## What is added

### 1. Light trails that behave at 60 fps (`RECOMP_FLARE_FIX`)

NFSU1 draws light halos (car lights, street lamps) into a 640x480 trail buffer that is never cleared.
Every frame it darkens the buffer by a fixed factor (0.51 / 0.59 / 0.71 per channel), adds the new
flares and a blurred copy of the scene, and adds the buffer onto the frame. The factor is per frame and
was tuned for the Xbox's 30 fps. At 60 fps a moving light leaves its copies twice as close together, and
they pile up into a big bright blob; oncoming headlights look like smeared floodlights.

With the fix, the decay is computed from the real frame time (`k_frame = k'^(dt / 33.3 ms)`), so a trail
lasts the same time at any frame rate, and the buffer is updated every frame, so the halo sits on the
lamp. By default the trail is also shortened to 10% of the Xbox's length and dimmed (`RECOMP_FLARE_TRAIL`,
`RECOMP_FLARE_TRAIL_OPACITY`; 1 and 1 give the Xbox's trail at the correct speed). The flares themselves,
their textures and their depth test are the game's.

### 2. A subtle glare on bright lights (`RECOMP_FLARE_GLARE`)

One full-screen pass adds a soft, warm (1.0, 0.95, 0.8) glare: four short streaks and a small halo. Its
only source is the trail buffer above (flares plus blurred scene), and only its brightest texels, so
lamps and oncoming headlights glare a little and nothing else does. Offsets are in the game's own pixels,
so the look does not change with the render scale. Off by default; every parameter is a switch.

### 3. Sharper car reflections (`RECOMP_VK_CUBE_UP`)

The car bodies reflect a cube map the game renders every frame. Its six faces follow the render scale
(768x576 each at 4K). With `RECOMP_VK_CUBE_UP=1.5` they are rendered at 1.5 times that, so the
reflections on the bodywork are less blurry. Same scene, same cube, only more pixels.

### 4. HD textures made from the game's own textures (`NFSU1_HD_*`)

The port already loads HD replacements for its interface art. The runtime part proposed here lets an HD
pack hold block-compressed textures with their own mip chains (NHD2: BC1/BC2/BC3), preload them into RAM,
and cover palettised P8 textures (the road surface, foliage, many props). **No textures are part of this
PR or of the repository.**

The pack used on the test console was made from **the game's own textures, upscaled by an AI model**
(Real-ESRGAN `realesrgan-x4plus`, by xinntao, BSD-3-Clause), not from a third-party retexture:

- Every texture was extracted from the user's own disc and upscaled 4x (small textures) or 2x (larger
  city textures, so the pack fits in memory), then block-compressed with its own mips.
- **Entries are reviewed automatically against the original** before they stay in the pack: the HD
  image is box-downscaled back to the Xbox size and compared with the original for colour (per-channel
  error, mean shift, PSNR), alpha (coverage and error), structure (SSIM of luma) and invented detail
  (high-frequency energy against a plain Lanczos upscale). Failing only on colour, the image is corrected
  (the residual is projected back); failing on structure or alpha, a plain Lanczos upscale is used
  instead; failing both, the entry is dropped and the game uses its own texture.
- **Lights are held to strict limits** (lamps, flares, glows, neon, lenses): one failed metric and the
  entry is dropped, so lights keep their original falloff and brightness.
- **Colour under mask alpha is checked on every mip.** Road and floor textures use alpha as a reflection
  mask, not transparency, so the colour under alpha ~0 is drawn. A separate check rebuilds or drops
  every entry whose colour there drifts (this caught black patches on the garage floor in play).

Result for the full pack: 3546 entries, about 533 MB of block-compressed data. For the city, loading and
menu textures: 2484 reviewed, 1493 passed as upscaled, 805 colour-corrected, 137 replaced by Lanczos, 49
dropped. For the P8 city textures: 280 files reviewed, 82 passed, 125 corrected, 12 Lanczos, 61 dropped
(47 of them lights). The low-alpha colour check then rebuilt 269 entries and dropped 26. The first batches
(car textures, 820 entries) were checked by eye on before/after crops before the automatic review
existed; the low-alpha check covered them too. The workflow is reproducible:
[HD_PACK_WORKFLOW.md](HD_PACK_WORKFLOW.md).

Known limits: car skins and wheels are composited by the game at run time, so they keep their original
look; some tiny noisy textures (for example the white reversing-light glass on one car) get small
invented scribbles from the model, visible only up close.

## Cost

Measured on the PS5 at 4K (render scale 4.5) with `RECOMP_FLARE_FIX=1`, `RECOMP_FLARE_GLARE=1`,
`RECOMP_VK_CUBE_UP=1.5` and an HD pack of 3301 entries with preload (415 MB), scripted Inner City race:
GPU busy 3.2-5.2 ms on average per 10-second race window, peak 5.9 ms (of 16.7); no race hitches; loading
the track took 184-201 ms on the loading screen, as before. A manual race with the P8 entries added
(537 MB preloaded): 3.0-5.1 ms average, peak 7.1 ms. The cost of each option on its own was not measured.

# PS5 build: licences and source

The NFSU1 PS5 host and its SDL3 display bridge are MIT.
SDL3 3.4.18 is altered by `ps5/sdl/SDL_ps5vulkan.c`,
`tools/rebuild-deps.py`.

The PS5 executable additionally links PS5 Platform from the pinned
PS5_PayloadSDK fork, and uses PS5_Vulkan's native link recipe, clean-room libc
module and C++ allocation runtime. These components are GPL-3.0-or-later;
the combined PS5 executable is subject to those terms. Its third-party notices
and the upstream MIT/LGPL, SDL zlib, glslang and SPIR-V notices accompany the
local build. Mesa contains several licences; its complete `licenses/` folder
and `docs/license.rst` accompany the build.

Source revisions and repositories are listed in `ps5/dependencies.json`.
Downloaded source stays in `build/deps/`. NFSU1 modifications stay in the
published host/runtime files and scripts; generated game C is unchanged.
The installed public PayloadSDK is not modified. No Sony SDK or system module
is distributed. `libc.prx` is the reproducibly generated clean-room runtime
from PS5_Vulkan, with its expected release SHA-256 recorded by the packager.
AGC link-stub bodies are host tooling and are not packaged or executed.

dav1d (VideoLAN and dav1d authors, BSD 2-Clause) decodes the upscaled movies. It is
built unmodified from the pinned revision by `tools/build-dav1d.py` and linked into
the executable; its licence accompanies the build as `dav1d-BSD.txt`.

The `hd/` folder of the public package holds the port's own art: PlayStation button icons
and a DualSense diagram from `ps5/assets/`, Conduit-style letters and signs drawn from
the strokes in `tools/font_strokes.py`, and Liberation Sans. No pixel of the disc is in
it; only the size and place of each glyph are read from the disc, to fit its cells.

`ps5/assets/fonts/liberation/` contains Liberation Sans Regular and Bold
2.1.5 (Red Hat, Google digitized data), SIL Open Font License 1.1; see its
`LICENSE` and `AUTHORS`. They are build-time outline sources for the HD Arial
font atlases and are not loaded by the game.

`ps5/assets/fmv-*.hashes` hold only hashes of decoded frames. The PS5 executable
contains the game's code statically translated to C; that translation is made
locally from the user's XBE and no licence here covers it.

The original game assets and Xbox executables remain the user's local input;
these source licences do not grant rights to those files.

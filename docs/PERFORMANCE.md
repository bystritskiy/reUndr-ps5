# Performance on the PS5: what was measured and changed

This page explains the performance changes proposed in this pull request: the problem they address,
how it was traced, the numbers before and after, what did not help, and what is still open. Everything
here was measured on one retail PS5 running homebrew, with this port and the user's own NTSC-U disc.
Numbers are from 1-2 runs per variant, so treat small differences as noise.

The game itself is still supplied by each user from their own disc; none of this changes that.

## The problem

The port already ran close to 60 fps at 4K. What remained were short, repeatable micro-hitches at the
same places every time:

- the start of a race, with lots of tyre smoke;
- some corners;
- small hills, where the car hops.

A lower render scale made it a little better but did not remove them. The goal was a steady 4K60
(render scale 4.5) with the picture unchanged.

## How it was measured

- `RECOMP_FPS_LOG=1` (existing): fps every 10 s plus `[hitch]` lines for long frames.
- A per-frame breakdown and a GPU timestamp counter (`[perf]`, `[gpu]` lines) and a 1 kHz sampling
  profiler of the guest threads. **These tools are not part of this PR** (they add code to hot paths);
  they were used to find the causes below.
- A scripted gamepad input (the existing `RECOMP_PAD_SCRIPT`) drove the same Quick Race (Olympic
  Square or Inner City, 2 laps, minimum traffic) for each variant, so runs are comparable without a
  person at the controller. Saves were hashed before and after every run; no run changed them.

## The investigation path

1. **First hypothesis: the render thread is the limit.** All NV2A-to-Vulkan translation runs on one
   executor thread, and the Switch port of the same runtime reports that thread near 95% busy. On the
   PS5 this turned out to be wrong at the time: the executor fit in the frame (11-16.5 ms including
   command recording, against 16.7 ms), and the **GPU was busy only ~2 ms per frame at 1440p and
   ~3.2-3.5 ms at 4K**. Hitches were not texture uploads (<= 3.4 MB in <= 2 ms per hitch frame) or
   shader compiles (0 in every race hitch: pipelines are cached and pre-warmed).
2. **The guest main thread was the limit**: 10-13 ms per frame in a race, with spikes of 37-105 ms in
   the hitch frames.
3. **The profiler found two runtime costs on that thread:**
   - **Emulated TLS.** Guest registers (`g_eax` ... `g_edi`, `g_esp`, FPU and SSE state) are `__thread`
     variables, and the PS5 payload SDK builds with `-femulated-tls`. Every guest register access was a
     call to `__emutls_get_address`, which calls into libkernel (`pthread_getspecific`). This was
     15-16% of race frames and most samples in hitch frames.
   - **A system call per guest clock read.** After native TLS, 76-81% of the samples in the remaining
     race hitches were in libkernel `clock_gettime`, reached from `QueryPerformanceCounter` (guest
     `rdtsc` emulation, ~110 reads per frame) and `GetTickCount64`. On the PS5 `clock_gettime` is a
     real system call: **20.3 us, against 11.8 ns for a TSC read**, measured by mihawk-99
     ([PS5_Vulkan `docs/HARDWARE_FINDINGS.md`, lines 3261-3266](https://github.com/mihawk-99/PS5_Vulkan/blob/f410e67a0241b3e8edcdbcd2a9b285dfc4a412ed/docs/HARDWARE_FINDINGS.md#L3261-L3266)).

## The fixes and their results

| Change | Switch | Measured on the PS5 | Before | After |
|---|---|---|---|---|
| Native TLS for the game and runtime targets | build | guest main thread, scripted race, 1440p | 10.2 ms/frame (max 12.5) | 6.1 ms (max 8.1) |
| | | p99 frame time, same runs | 24 ms | 19 ms |
| | | hitches in a manual Inner City race | 5-8 per run, up to 131 ms | 2 (51 / 68 ms) |
| Fast guest clock from the TSC | `RECOMP_FAST_TIME` (proposed **on**) | guest main thread, scripted Inner City race | 6.75 ms (max 8.6) | 5.65 ms (max 6.5) |
| | | race hitches with guest > 30 ms, same runs | 2 (52 / 59 ms) | 0 |
| | | clock error vs `clock_gettime` (self-check) | - | 2.7-4.5 us |
| Crash report written straight to `stderr.log` | always | robustness | log ended in `[CR` | full report |

Together: the guest main thread in a race went from **10-13 ms to 5.5-6.5 ms per frame**, and a manual
4K race ran at **57.8-59.9 fps in most 10-second windows** with the GPU busy ~3.3 ms of 16.7.

How the fixes work:

- **Native TLS** (`ps5/CMakeLists.txt`, `tools/link.sh`): `-fno-emulated-tls -ftls-model=local-exec` for
  the `nfsu1_*` targets only; SDL, dav1d, RADV, glslang and libc++ stay as they were. The pinned
  linker script has no `PT_TLS`, so `link.sh` generates a copy with one, places `.tdata/.tbss` ahead of
  `.data` and aligns them to 32 (the converter's minimum, which fixes the static TLS offset); the link
  fails unless the result has `PT_TLS` align `0x20`. A boot line `[TLS] native: main fp_cw=0x37f thread
  fp_cw=0x37f` proves the loader placed the TLS image where lld assumed. `__emutls_get_address` calls in
  the eboot: 6752 -> 16. The runtime's 64 KB per-thread path buffer moved to the heap, because with
  static TLS it would sit in every host thread.
- **Fast clock** (`platform/fast_time.h`): `ns = anchor_ns + (rdtsc - anchor_tsc) * scale`. Whichever
  thread finds the anchor older than 2 ms refreshes it from `CLOCK_MONOTONIC` (try-lock, nobody waits),
  readers use two seqlock'd slots, the scale is re-measured every >= 100 ms, and the value handed out is
  an atomic maximum, so time never goes backwards. A TSC reset or jump against `CLOCK_MONOTONIC` (rest
  mode) re-anchors and is counted. `QueryPerformanceCounter`, `GetTickCount64` and
  `GetSystemTimeAsFileTime` read it, so every guest time source stays on one clock in the same units.
  Boot line `[time] fast on|off`; with it on, a `[time] fast on: ...` self-check every 10 s.
  `RECOMP_FAST_TIME=0` restores the old `clock_gettime` code exactly.
- **Crash report**: `stderr` is a pipe to a writer thread on the console, so a crash killed the process
  before the report reached the file. With `g_crash_log_fd` set the handler flushes, gives the writer
  ~150 ms, writes past the pipe straight into the log file and `fsync`s it.

## What did not help (measured, not proposed)

These were implemented and measured on the console, then left out of this PR. Each row says whether it
was measured before the TLS fix (when the guest thread was the limit) or after it.

| Experiment | Result |
|---|---|
| Second render thread (executor decodes, a recorder thread records Vulkan commands) + executor/recorder pinned to their own CPUs + CPU-side redundant-state trims + vertex cache, all on, before TLS | heaviest race windows 46.5 fps vs 54.5 with all off; guest-lock waits 2.3-5.6 vs 1.9-2.8 ms |
| The same without the second render thread, before TLS | guest-lock waits as with all on (3.2-4.2 ms); one crash mid-race (cause not found: the crash log was cut off, which is why the crash-log fix exists) |
| Fewer GPU barriers (29 -> 19 per frame), after TLS | GPU time 3.2 -> 3.2 ms at 4K: no gain |
| Skipping the guest-lock hand-over in poll loops when nobody waits, after TLS | guest 6.39 -> 6.39 ms, lock wait 0.62 -> 0.62 ms |
| Yielding the guest lock in the `rdtsc` emulation, after TLS | 4 hitches with it vs 2 without (two manual runs, not identical) |
| Culling invisible batches (`RECOMP_CULL=1`, existing), before TLS | only 18-25 of 2290-3299 draws per frame are off screen |

Read, not measured: the PS5_Mesa commits after the pinned one change memory handling, not speed.

## Open avenues and recommendations

1. **Re-test the second render thread and the trims now.** After the TLS fix the limit moved: the
   executor thread is busy **12-15 ms per frame on average, with peaks up to 17.8 ms**, right at the
   16.7 ms budget. The earlier verdict on splitting it was made while the guest thread was the limit.
2. **`recomp_spin_wake` broadcast cost.** On a Linux host the profiler put 52-78% of the busy guest-main
   samples in libc under `recomp_spin_wake` (waking NV2A threads from poll loops: a condvar broadcast
   every 16 spins). Not measured on the PS5.
3. **Loading screens.** Loading a track with HD textures takes 184-201 ms in one frame (the same
   ~190 ms as without the changes here). It happens on the loading screen, not in a race, but a
   background upload would hide it.
4. **Car skins and wheels in HD.** The game composites car skins (512x512) and wheel atlases at run time
   in P8, so their indices exist in no file on the disc and an HD pack cannot key them. A dump of the
   composed skin (`NFSU1_HD_P8=1` + `NFSU1_HD_DUMP`) works for one car, paint and vinyl at a time.
5. **Fog.** The vertex program epilogue uses `oFog.x`, but 35 of the 51 race vertex programs logged
   write only `oFog.w`, so most of the world is drawn without the game's distance fog. A candidate fix
   (read `oFog.w` when `.x` is never written) darkens distant buildings as expected on the host, but
   which component the real NV2A uses is not verified against hardware or xemu, so it is not in this PR.
6. **Reflection shimmer.** Some flicker in the car reflections near street lamps was seen in play; not
   investigated.

The measurement tools (per-frame breakdown, GPU timestamps, sampling profiler with a report script, and
the scripted console run harness) and the experiment code can be offered separately if the maintainers
want them.

## 120 fps

Measured 2026-11-26 on the same PS5 with a 4K TV that takes 120 Hz with VRR (48-120 Hz), render
scale 4.5, FXAA, 16x filtering, reflections, detail 8x. Races were driven by `NFSU1_PAD_SCRIPT`
(Circuit, default car and track, throttle with some steering), so runs repeat; numbers are the
10-second windows of `RECOMP_FPS_LOG` from 80 to 260 s after start.

### Output at 119.88 Hz

`param.json` asks for 119.88 Hz and PS5_Mesa configures it when it opens VideoOut, then keeps it
only if a vblank comes within 12.5 ms. It measures before anything is flipped, and a VRR display
idles at its floor: the console logged `119.88Hz request is converted to VRR 119.88Hz`, Mesa measured
20.87 ms (48 Hz) and went back to 59.94 Hz at every start. `ps5/src/video_hz.c` wraps
`sceVideoOutOpen` and `sceVideoOutConfigureOutput` (`--wrap` in `tools/link.sh`), holds that
restore, and measures again while the game flips at 120: 8.34 ms, so the output stays at 119.88 Hz.
A display without 120 Hz still gets Mesa's restore. `NFSU1_VIDEO_HZ=60` lets it through at once.

Mesa reads the 119.88 Hz flag from `/app0/sce_sys/param.json`. An installed FPKG has no such file
(only `about/`, `keystone` and `pfs-version.dat` are left in its `sce_sys`; the system keeps
`param.json` in `/user/appmeta`, which the title cannot read), so Mesa never asked for 119.88 Hz.
`video_hz.c` then writes `/data/reUndr/videoout.json` with the flag, points
`PS5_VIDEOOUT_PARAM_JSON` at it and deletes it once VideoOut is open.

### Game time

The guest vblank follows the frame rate (`xbox_vblank_set_hz`: 119.88 or 59.94 Hz), but the game's
frame step (`sub_001410D0`) clamps each frame to at least 1/60 s and its world loop (`sub_00141200`)
runs at least one 60 Hz tick a frame: above 60 fps the world ran fast (a lap timer gained 20.25 s in
16.6 s at 72 fps). At 120 the floor is 1/240 s and a frame may add no tick; the lap timer then runs
1.0167x real time, the game's own factor. Replays read their steps from the stream and are left as
they are. Below 120 both functions are the original.

### Where the time went

At the first 120 fps build a race driven by hand ran **70-84 fps**. The guest main thread, the pushbuffer executor
and the GPU all had room on average; the limit was the executor, which decoded the pushbuffer and
recorded every Vulkan command on one thread (`RECOMP_PROF`, a 1 kHz sampling profile of the three
threads, and per-frame timers that were removed again). The changes, in the order they were made:

| Change | Race windows: min / median |
|---|---|
| Baseline at 120 (scripted race) | 89 / 106 |
| Vulkan recording on its own thread (`RECOMP_VK_THREAD=1`, from the Switch port's queue) | 96 / 114 |
| Lifted game at `-O2 -march=znver2` instead of `-O1` | 102 / 119.1 |
| Queue and recording trims, native maths, guest lock spin, plain memory for `.text` (below) | 113.7-117 / 119.9 |

The last row is two runs of the final build: 16 of 18 windows at 119.5 or more in each, the slowest
113.7 and 117.0, both at the race start. The same build with `RECOMP_VK_THREAD=0`: 12 of 18
windows at 119.5 or more, the slowest 113.5, with dips all through the race. The last row's parts:

- **Queue between the executor and the recording thread** (`nv2a_vk.c`): draws are published eight at
  a time and the consumer releases space eight records at a time (each publish moved a cache line
  between cores); records are fixed up in place instead of copied; the next record is prefetched.
  Transform constants and programs go as the rows the executor marked (`nv2a_pb_vp_dirty`) instead of
  comparing all 192 rows each draw, and register blocks are scanned eight at a time.
- **Recording** (`nv2a_vk.c`): a texture unchanged for 120 frames is hashed every 8th frame
  (staggered) instead of every frame; at 120 fps the race's game-drawn P8 pair, rewritten every frame,
  is decoded every second frame (it then changes at 60 Hz, as the game drew it); render-target lookups
  by address are memoised; absent vertex attributes reuse their ring block and only changed vertex
  buffers are bound; index copies vectorise (`__restrict`, the build has `-fno-strict-aliasing`).
- **Guest main thread** (`recomp_manual.c`): native versions of the hottest library routines (4x4
  multiply, `_ftol2`, matrix times vector, AABB transform, frustum test), the same arithmetic in the
  same order. Generated files 0000-0047 (the game, CRT and maths) access guest memory without
  `volatile`; the D3D and DirectSound files, which hold every poll loop, keep it.
- **Guest lock** (`kernel_bridge.c`): the main thread spins 80000 TSC ticks before it sleeps on the
  lock; a condition-variable wake cost it 0.6-0.9 ms a frame.

Frame Rate in the port menu (Graphics tab) switches at once between 120, 60 and 30; 120 is offered
only on a 119.88 Hz output. At 60 on a fixed 120 Hz output each flip is held for two vblanks
(`wsi_videoout_set_flip_rate`), so 60 stays even.

### Not helped

| Experiment | Result |
|---|---|
| Pinning the guest, executor and recording threads to cores (0/2/4 or 8/10/12) | 0-60 fps in the menus; the race was never reached |
| Four swapchain images instead of three | same windows, same dips |

### Open

The slowest windows are the race start (frame ~14170, the same place every run): 113-118 fps. That
one is GPU-bound: up to 7.7 ms of GPU time a frame at 4K. At render scale 3 every window was 119.9.

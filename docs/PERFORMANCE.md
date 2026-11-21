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

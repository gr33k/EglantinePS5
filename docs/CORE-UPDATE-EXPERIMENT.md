# EglantinePS5: core update experiment (Eglantine upstream babdb6bb75)

2026-11-11. Worktree `~/Code/EglantinePS5-wt-core`, branch `exp/core-update`; since then the result is the in-repo `Eglantine/` (branch `core/in-repo`, Eglantine/UPSTREAM.md), where the patch files below became ordinary commits. The submodule is on branch `exp/core-update-patches`. Nothing is installed: every console run went through the runner, which put main's release back afterwards.

## Summary

- **It builds and runs Zelda stably at the game's 30 FPS cap**: two full runs on build u16 held **29.6–29.7 FPS** on Link's porch (HUD "F31"), against **~20.5 FPS on main** (cec2d7e, same benchmark).
- Getting there took 8 fixes for upstream changes that break the PS5 (listed below): 5 for memory, and 3 for deadlocks between the GPU threads and the GPU memory service, found with a new hang-stack watchdog.
- Memory is ~0.5 GB tighter than on main (~0.7 GB free in gameplay vs ~1.2 GB).

## What the branch contains

- **Core:** upstream Eglantine nx2 **babdb6bb75** (1,175 commits after our pin 3140648a66).
- **Ported patches:**
  - applied as-is: 0016, 0021, 0030, 0034, 0055, 0057;
  - adjusted: 0031, 0015 (NIR interface only), 0024 (texture cache ceiling).
- **Not ported:** the other ~65 patches. Upstream already covers most of what they fixed, but differently. Missing in particular:
  - the Zelda series 0054/0068/0070–0078;
  - the MKW memory work 0004/0010–0012/0023/0058/0059.
- **Dynarmic** stays at the old pin; upstream's JIT needs a new PS5 port.
- **PS5 layer:**
  - built in "lenient" mode;
  - 55 Python and 11 CMake port edits no longer match upstream and were skipped;
  - the multiplayer sockets use upstream's code.
  - Lists: `.local/core/skipped.txt*`.

## Fixes needed on top of upstream (all on the experiment branches)

1. **Fiber stacks.** Upstream rewrote `fiber.cpp`, so our PS5 stack edit silently missed. Each guest thread held a 2 MiB heap block: **330 MiB** for Zelda's 165 threads. Fixed in `headless/CMakeLists.txt`, which now also fails loudly if this edit misses.
2. **Turing macro JIT (new upstream, on by default).** The console can't give it executable memory: crash "Xbyak can't alloc" 29 s in. Now off by default, as on main (`EDEN_NX2_MME_JIT=1` turns it on).
3. **Uneven-mip ASTC kept as RGBA8 on AMD** (upstream #679, "not run on AMD hardware"). On the PS5's RADV almost every texture qualifies, so 4.7 GB of images and OOM at 55–90 s. Now off by default (`EDEN_NX2_UNEVEN_MIP_RGBA8=1` turns it on).
4. **ASTC→BC transcode scratch.** Upstream keeps one RGBA8 image per texture shape for reuse, with no limit (up to 192 MiB each, 3 GB during the load). Ported main's cap: **64 MiB** in flight, flush every 32 MiB.
5. **Texture cache ceiling (0024).** 3584/4096 MiB instead of upstream's 4352/5120.

6. **Deadlock: GPU memory registry.** `Nx2GpuMemoryRegistry::MapAt`/`Unmap`/`UnmapMemory` called their listeners while holding the registry mutex. The listeners queue work for the GPU thread, and that queuing blocks while the GPU command queue is full. Meanwhile the GPU back thread, flushing an evicted texture into a sparse region (`BackSparseWrite` → `BackSparse`), needed that same mutex. Now the listeners run after the mutex is released.
7. **Deadlock: GPU address space.** `nvhost_as_gpu::Nx2Map`/`Nx2Unmap`/`Nx2Release`/`Nx2Sparse`/`Nx2SparseTile` held their mutex across `GPU::MapInOrder`/`RunInOrder`, the same cycle on a second lock (the back thread waits in `Nx2BackSparse`). They now release it before queuing.
8. **Deadlock: nx2:gpu handlers** (102, 213, 214, 216). They held the session's region lock, which the back thread takes in `BackSparseWrite`, while the registry's listeners and their own `RunInOrder`/`SignalAfterGpu` queued GPU work. Same class of cycle; no run had hit it yet. Now a `Tegra::DeferGpuQueue` opened before the lock holds those pushes and makes them, in order, once the lock is released (`MapInOrder` ranges still count as pending at once).

Also kept: the 8 MiB cap on read-only native compute storage reads (upstream #1231 allows up to 1 GiB). It turned out not to be the OOM cause, but it matches main.

## Console runs (Zelda, save loads on Link's porch, 1x, 1080p)

| Run | Build | Result |
|---|---|---|
| zcore1, zc5, zc7–zc10 | u4–u10 | Out of device memory at 55–90 s (`BufferCache::CreateBuffer`) |
| zc6 | u6 | Macro JIT "can't alloc" at 29 s |
| zc11 | u11 | Heap exhausted at 57 s |
| zc12, zc12t | u12 (256 MiB cap) | Survived, but thrashing: 0.3–1.5 FPS |
| **zc12p** | u12 + 64 MiB cap + 0024 limits | **Gameplay, stable for 80 s: 28–30 FPS** |
| zr1–zr3, zr5, zr6 | u13–u15 | Froze at the scene load (~50 s): the deadlocks above |
| **zr7, zr8** | **u16** | **Stable for the whole run (180 s): 29.7 and 29.6 FPS, HUD F31** |
| **zr9 (×2)** | **u17** (+ fix 8) | **Stable: 29.8 and 29.5 FPS**; one run had a 14 s slow stretch at the load (waiting on a present), then recovered |

**Measured, u16 (zr7, zr8) vs main** (main = runs znl_1 and zl949_1 of cec2d7e's series):

| | Experiment | Main |
|---|---|---|
| FPS (fps.py, last 39 s) | **29.7 / 29.6** | 20.7 / 20.1 |
| Presented FPS (5 s windows) | steady 28.5–30.4 | 19.7–21.1 |
| HUD | F31 | F21 |
| GPU driver memory | 3.75–3.8 GB | ~3.9 GB |
| Game memory | 4.46 GB | ~5.0 GB |
| Eden heap | 1.25–1.28 GB | 1.45 GB |
| Pool free | **~0.68–0.75 GB** | ~1.2 GB |

The image looks right (lit scene, foliage). One run (zc12p, before the deadlock fixes) showed a ~64 px strip of stale content on the left edge; the u16 frames are clean.

## How the freeze was found

- `headless/hang_stacks.h` adds a watchdog, enabled with `EDEN_HANG_STACKS=1`. It notes every named thread and its stack. When the GPU-side heartbeat stops for 8 s, it signals each thread for `rip`/`rsp`, scans its stack for return addresses, and prints them to stderr.
- `tools/symbolize-hang.py STDERR ELF` names them.
- ps5debug-NG's debugger could not list the threads of the hung app: its stop-and-wait broke the session.

## Takeaways

- Upstream's CPU-side work gives about **+45% on Zelda** (it reaches the 30 FPS cap). That is more than all our recent CPU-side patches together.
- A real move to upstream needs:
  - a new PS5 port of dynarmic;
  - the 66 skipped port edits redone;
  - the Zelda and MKW memory work redone on upstream's structures;
  - upstream's AMD and texture defaults checked against the PS5.
- Estimated at several days, not hours.

Builds: `.local/core/candidate-u17` (latest, zr9) and `candidate-u16` (zr7/zr8). Rebuild with `.local/core/build.sh NAME`. Trail: `.local/core/STATUS.md`.

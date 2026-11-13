# Eglantine/ and its upstream

`Eglantine/` is the emulator core of EglantinePS5. It is our code: it lives in this repository and is
changed directly, in ordinary commits, like `headless/`.

## Where it came from

Imported once, squashed, from Zortos's Eglantine fork (a Switch 2 capable fork of Eden):

- Repository: https://github.com/zortos293/Eglantine, branch `nx2`
- Commit: `babdb6bb752af8a35e94b305613d6be7ba7e6217` (2026-11-11)
- Import commit in this repository: "Eglantine: import upstream nx2 babdb6bb75 as in-repo code
  (squashed)", whose `Eglantine/` tree is upstream's tree byte for byte.

Before the import, `Eglantine/` was a submodule pinned at `3140648a66` with the patch series
`tools/eglantine-patches/`; that history stays in git (the branches before the import). The
commits right after the import are the core update experiment's changes (docs/CORE-UPDATE-EXPERIMENT.md),
applied as ordinary commits with their messages.

## Upstream from here on

The PS5 port's changes to the core are part of these sources too: they once were text edits the
build made at configure time (tools/prepare-vulkan-port.py, headless/*.cmake) and are now written
here. Where the PS5 build, the host's test build (`make test`) or a build switch need different
code, a chain of conditionals marks it, each line ending in a fixed comment:

    #if !defined(__PROSPERO__) // not PS5
    #elif defined(EDEN_DEV_PROFILE) // PS5 development build
    #else // PS5
    #endif // PS5

(tools/eglantine_source.py reads a source as one configuration compiles it; the switches are
compile definitions of every target, headless/inject.cmake.)

There is no submodule, subtree or patch series to keep in sync: upstream diverges a lot from what
the PS5 needs, so its changes come over by hand, one at a time, where we want them. To look at what
it has done since:

    git fetch https://github.com/zortos293/Eglantine nx2
    git log --oneline babdb6bb75..FETCH_HEAD                        # what came in upstream
    git format-patch -1 --stdout <commit> | git am -3 --directory=Eglantine   # one commit, or port it by hand

When you bring one over, name it in the commit message (`upstream <hash> (#PR)`); later changes,
ours and upstream's, are in `git log -- Eglantine`.

## Not done yet

- dynarmic (`Eglantine/src/dynarmic`) is kept at the old pin's version (its PS5 port needs it).

## Our changes right after the import

- 0016-integer-add-overflow
- 0021-window-stack-pages
- 0030-uniform-prefetch
- 0031-macro-state-writes
- 0034-split-channel-init-on-gpu-thread
- 0055-open-file-report
- experiment: keep dynarmic at the old pin 3140648a66
- 0015-nir-compute-backend (adapted: NIR handoff interface and detach only; compute stays SPIR-V)
- experiment: Shader::Info::uses_tensor_mul_add for the PS5 NIR backend
- experiment: StalePages page size not named PAGE_SIZE (a macro in the PS5 SDK headers)
- 0031 adjusted: upstream's trace_first_method_writes; no RtDump (patch 0004 is not in)
- experiment: PS5 libc++ std::max type (nx2_gsv vsync trace)
- 0057-svc-memory-stats
- experiment: carry the loading-probe and readback-dependency diagnostic headers (from 0004) for the PS5 layer
- experiment: RasterizerDownloadArea::completed_generation for the PS5 read trace
- experiment: keep the 8 MiB cap on native compute storage reads (PS5 shared pool)
- experiment: Turing macro JIT off by default on the PS5 (no executable memory for xbyak)
- experiment diag: report buffer-cache buffers of 64 MiB and more on stderr
- experiment diag: report Vulkan images and buffers of 8 MiB and more on stderr
- experiment: uneven-mip ASTC stays BC3 on the PS5 unless EDEN_NX2_UNEVEN_MIP_RGBA8=1
- experiment: bound the ASTC transcode decode images in flight (PS5 shared pool)
- experiment: 0024 texture cache ceiling ported; transcode bound 64 MiB by default
- experiment: Nx2GpuMemoryRegistry calls map/unmap listeners outside its mutex (GPU split deadlock)
- experiment: ListenerSnapshot return type deduced (Listeners is declared later)
- experiment: nvhost_as_gpu releases its mutex before queuing GPU work (GPU split deadlock)
- experiment: nx2:gpu handlers queue GPU work only after releasing the region lock (Tegra::DeferGpuQueue)

## Upstream commits newer than the import (at the time of the import)

- `4e7d87aca3` yuzu: frame stats overlay redesign (big fps, CPU/GPU %, 5 s frame-time graph) (#1345)
- `752328bf38` loader: MKW dynamic FPS skips the game's own second-vblank wait; split GpuSplit front/back cache lines (#1332)
- `b4b830c8fa` vulkan: back images over the texel limit that fit in 1 GiB (#1325)
- `be24147966` vulkan: build evicted graphics pipelines again from kept SPIR-V instead of translating them (#1337)
- `fd9c0133df` dynarmic: give each processor's exclusive reservation its own cache line (#1331)
- `e63d0a04df` vulkan: keep graphics pipelines up to a RAM limit instead of 9000 frames, and an option to build all cached shaders at boot (#1334)
- `8e5259aa7b` vulkan: skip FlushCommands' submit when nothing was recorded (port of Fritchii EXP61) (#1329)
- `20621cda95` dynarmic: plain loads for LDAR, LDAPR and LDAXR, no barrier for DMB LD/ST on x86 (port of #161) (#1327)
- `b933bc145a` AGENTS.md: add Metroid Prime 4 NS2 Edition and Mario Kart 8 Deluxe program ids (#1330)
- `43af97ffaf` shader_recompiler: reduce shader compilation work (#1320)
- `933d35ff00` vulkan: allow device memory overallocation on AMD drivers (#1335)
- `d42e907107` buffer_cache: size read-only native compute bindings from their size word instead of the whole mapping (#1333)
- `0490f57b9d` Native HDR output (HDR10/scRGB passthrough, Off/Auto/On) (#1326)

# SimTextureStrm – Terrain Texture Upload Prototype Plan

SimTextureStrm is a testbench for prototyping the terrain-tile texture upload path of a Level D whole-world flight simulator (UE 5.6, DX12, Large World Coordinates, deferred) before porting to the product.

The prototype only covers **textures**. Static mesh component (SMC) management and tile geometry are fixed scaffolding and are not optimized here.

## Goal

Replace the current per-swap `UTexture2D::UpdateResource()` path with per-size pools of pre-allocated `UTexture2D` slots that are updated **in place** (`UpdateTextureRegions`, later a custom `ENQUEUE_RENDER_COMMAND` uploader). Then measure the before/after effect on worst-case frame time, for a strict deterministic 60 Hz at 4K per channel.

## Product facts (context for decisions)

- Terrain textures arrive from a terrain database server **already DXT1 (BC1)-compressed, with precomputed mip chains** (mips must match neighbouring tiles).
- Textures are **opaque RGB**.
- Texture LOD is **independent of geometry LOD**. Split triangle tiles keep sharing the parent texture with adjusted UVs.
- Texture resolution steps are always one power of two at a time (512 → 1k → 2k → 4k and back).
- Production hides the pop with a mip-clamp fade (clamp to mip 1, then blend to mip 0), using world samplers plus a custom HLSL node, with hardware anisotropic filtering. **The prototype does not implement the fade; popping is acceptable.**
- Texture streaming is **off**. Tiles are cached on the CPU to avoid re-requesting from the server.
- There is one material instance (MID) per tile SMC, created up front and reused.
- Target hardware: RTX PRO 6000 Blackwell class, one box per channel. The channels may be swap-locked, so the worst box sets the frame for everyone.
- Dev hardware: RTX 3080 (10 GB), Ryzen 9 5900 (12 cores), 32 GB RAM. Numbers from the dev PC are for **relative** comparison only.

## Settled decisions

1. Per-size pools of `UTexture2D` slots (buckets by size/format/mips/sRGB). No texture arrays and no atlases.
2. `UpdateResource` is called **once per slot at startup** and is **banned on pooled slots afterwards** (enforced by a check).
3. In-place updates are whole-mip regions starting at (0,0). DXT1 pitch = `max(1, w/4) * 8` bytes, mip size = `max(1, w/4) * max(1, h/4) * 8` bytes.
4. The source buffers and region arrays passed to `UpdateTextureRegions` are heap-allocated, pooled, and returned through the cleanup callback.
5. Readiness and reuse are **event-driven** (cleanup callback or render command fence), not a fixed frame-count wait.
6. The free list becomes an **LRU**, plus a map from (tile, resolution) to slot, which gives a GPU-side cache for free.
7. MIDs stay one per tile, created up front and reused. Only the texture parameter changes.
8. Benchmarks run standalone (`-game`) with Lumen, ray tracing and Virtual Shadow Maps disabled, to isolate texture costs.
9. The scene is **static**. Updates are driven by knobs (burst / spread / pattern / target selection), not by camera flight.
10. The success metric is **worst-case** frame time (max, 99th percentile, 99.9th percentile, frames > 16.67 ms) on the game, render and RHI threads and the GPU. Averages alone are not a result.

## Working rules

- **One phase at a time.** After each phase, stop for user review and validation, and do not start the next phase until the user says so.
- Build with `.\BuildGame.cmd` only. Do not modify `BuildGame.cmd`, `LaunchProject.cmd`, `LaunchProject.ps1` or `LaunchGame.cmd`.
- The user creates content assets (level, material, Blueprint subclass) by hand in the editor, following the phase docs.
- Code: UE coding conventions, `STS` prefix for types, `sts.` prefix for console variables and commands.

## Problem size (default knobs)

| Resolution | Tiles | BC1 + mips per tile | Working set |
|---|---|---|---|
| 4096 | 100 | ~10.7 MB | ~1070 MB |
| 2048 | 200 | ~2.67 MB | ~533 MB |
| 1024 | 300 | ~0.67 MB | ~200 MB |
| 512 | 400 | ~0.17 MB | ~67 MB |
| **Total** | **1000** | | **~1.9 GB** |

Pools (from Phase 3) default to 1.5× the working set per bucket, about 2.8 GB of VRAM.

## Phases

### Phase 0 – Static harness
Specification: [`Phase0_Harness.md`](Phase0_Harness.md).

A static scene of 1000 procedural triangle tiles (configurable), with a procedural DXT1 texture per tile (colour-coded by resolution, borders, mip coding). Tile creation and destruction can be toggled. All knobs are on a placeable actor. It also includes a benchmark recorder (CSV plus percentile summary), an on-screen HUD, and `RunBenchmark.cmd`.

**Milestone 0:** correct visuals, and noise-floor recordings **before and after** tile creation.

### Phase 1 – Synthetic terrain server and update knobs
- Worker-thread "server" that answers (tile ID, resolution) requests with DXT1 payloads plus full mip chains, after a configurable delay. Payloads are served from a pre-generated library (memcpy), so generation cost doesn't pollute measurements.
- Knobs:
  - **Burst:** a count per resolution, all in one frame.
  - **Spread:** swaps per second over a duration.
  - **Swap pattern:** same resolution with new content, one power-of-two step up or down, or mixed.
  - **Targets:** random, a ring, or a contiguous patch (simulated ring shift), with a fixed seed.
- A payload generation stripe, so a same-resolution swap is visible.
- Scenario sequences (warm-up → burst → pause → spread) with automatic recording, for repeatable runs.
- No new upload path yet. Updates are requested but only logged/queued until Phase 2.

**Milestone 1:** the knobs produce the expected request streams, and payloads are verified (checksums and visual check).

### Phase 2 – Baseline (the current production path, i.e. the "before")
- Pooled `UTexture2D` objects in a circular FIFO queue (return at one end, take from the other).
- A background thread builds `FTexture2DMipMap` data, and the game thread copies it into `PlatformData`.
- `UpdateResource`, a wait of N frames (knob), then the MID texture parameter is swapped.
- Selected with `-StsMode=Baseline`.

**Milestone 2:** "before" numbers for every test scenario.

### Phase 3 – Per-size pools + `UpdateTextureRegions`
- One pool per resolution. Each slot calls `UpdateResource` exactly once at startup, then `PlatformData` is released.
- One `UpdateTextureRegions` call per mip, each a whole-mip region with the DXT1 pitch.
- Pooled source buffers and region arrays, returned by the cleanup callback.
- A check that fires on any `UpdateResource` call on a pooled slot.
- The reuse policy **stays FIFO**, so this phase isolates the upload path change.
- Selected with `-StsMode=Regions`.

**Milestone 3:** tail mips (2×2, 1×1) verified, and a direct comparison against Phase 2.

### Phase 4 – Event-driven completion, upload budget, GPU cache
- Cleanup-callback / fence completion replaces frame-count waits, for visibility and for releasing the old slot.
- A per-frame upload budget (`sts.UploadBudgetMB`), filled by priority (nearest-first or a synthetic priority).
- An LRU free list per pool, plus a map from (tile, resolution) to slot, gives GPU cache hits with no upload. The CPU cache stays as a second level.
- The recorder adds cache hit rate, queue depth, and budget-induced latency.

**Milestone 4:** worst-case frame times under burst tests, compared with Phase 3.

### Phase 5 – Custom `ENQUEUE_RENDER_COMMAND` uploader
- All mips of a texture in one render command, calling `RHIUpdateTexture2D` directly.
- Budgeting on the render thread, with one completion signal per texture.
- Selected with `-StsMode=Custom`. Compared against Regions mode.

**Milestone 5:** a decision on which uploader to port.

### Phase 6 – GPU profiling of the upload copies
- PIX plus Unreal Insights GPU captures of a burst frame.
- Measure the graphics-queue time of texture-region copies and their resource state transitions.

**Milestone 6:** a go/no-go on copy-queue work. It is only worth pursuing if the measurement shows a real cost, roughly > 0.5 ms per frame within the budget.

## Test scenarios (from Phase 2 onward)

| Test | What it stresses |
|---|---|
| Steady spread (e.g. 5 swaps/s mixed) | Steady-state cost |
| Spread sweep (×2, ×5, ×10) | Throughput |
| Burst (e.g. 20×4k + 40×2k in one frame) | Hitches, allocation spikes, VRAM spikes |
| Oscillation (same tiles stepping up/down repeatedly) | GPU cache hits, re-upload waste |
| Soak (30–60 min mixed) | Memory growth, fragmentation |

Each test is run twice: uncapped with VSync off (for costs), and capped at 60 fps (for missed frames).

## Deferred / out of scope (revisit only if measurements demand it)

- Copy-queue uploads (depends on Phase 6).
- Async GPU texture creation with a streamer-style swap (a custom `FTextureResource`).
- Streaming Virtual Texture with a custom producer.
- D3D12 reserved (sparse) resources.
- Clipmaps, and texture streaming with a custom mip provider (both rejected for this product).

# Phase 3 – Per-Size Pools with In-Place `UpdateTextureRegions` (specification)

Parent plan: [`PrototypePlan.md`](PrototypePlan.md). Builds on Phases 0–2 ([`Phase0_Harness.md`](Phase0_Harness.md), [`Phase1_TerrainServer.md`](Phase1_TerrainServer.md), [`Phase2_Baseline.md`](Phase2_Baseline.md)).

Phase 3 adds an upload mode that **never reallocates GPU textures after startup**. Every pool slot is created once, and from then on it is updated in place, one whole mip at a time, with `UTexture2D::UpdateTextureRegions`. The response buffer from the server is the upload source directly: no mip builder and no `PlatformData`.

Phase 3 also fixes two measurement problems found in the first Phase 2 run, and adds one Baseline variant.

## 0. Findings from the first Phase 2 run (PIE, Burst) that shape this phase

Verified against the UE 5.6 source:

1. **`UpdateResource()` copies every mip on the game thread.** `UTexture::UpdateResource` → `CreateResource` → the `FTexture2DResource` constructor (`Rendering/Texture2DResource.cpp`) → `UTexture2D::GetInitialMipData` → `FTexturePlatformData::TryLoadMipsWithSizes` → `FBulkData::GetCopy(&Dest, true)`.
   - Runtime-created bulk data cannot be discarded unless it has `BULKDATA_SingleUse` or `BULKDATA_AlwaysAllowDiscard` (`FBulkData::CanDiscardInternalData`), so `GetCopy` does **malloc plus memcpy** of every mip.
   - The run measured 44–52 ms of `UpdateResource` game-thread time per burst frame (about 0.8 ms per 4k texture).
   - The baseline therefore copies each payload three times: builder (server buffer → bulk data), game thread (bulk data → `MipData`), and render thread (`MipData` → upload heap, plus the GPU allocation).
2. **The Phase 1 checksum cost 37–48 ms on burst frames.** That is harness cost, and it must not be in performance runs.
3. **`RenderThreadMs` read about 0.001 ms and `RHIThreadMs` about 0.16 ms for the whole Phase 2 run**, while the Phase 1 run of the same scene read 4.96 / 2.62 ms. `RTCreateMs` was non-zero (up to 67 ms per frame), so the render thread was working. The per-frame thread-time capture is unreliable and must be fixed before any comparison. In 5.6, `GRenderThreadTime` is written in `SlateRHIRenderer.cpp`.
4. **D3D12 in 5.6 already writes whole-mip updates directly into upload memory.** `FD3D12Texture::UpdateTexture2D` (`D3D12Texture.cpp`) uses `Lock`/`Unlock` of the mip when the command list is immediate and the region covers the whole mip; otherwise it uses a fast-allocator staging buffer. Either way it memcpys rows straight from the source pointer, so **there is no extra intermediate CPU copy**. The planned Phase 4 `CustomLock` variant is redundant (see [`PrototypePlan.md`](PrototypePlan.md)).
5. **`FD3D12Texture::UpdateTexture2D` checks that region width/height/offsets are multiples of the block size** (4 for DXT1). Regions for the 2×2 and 1×1 tail mips must be **padded to 4×4** (one block). Their source pitch is 8 bytes.
6. **`UTexture2D::UpdateTextureRegions` details.** It captures the texture's `FTexture2DResource*` on the game thread, calls `RHIUpdateTexture2D` per region on the render thread, and runs `DataCleanupFunc` **on the RHI thread** (`RHICmdList.EnqueueLambda`). So:
   - the resource must never change while updates are queued (hence the `UpdateResource` ban);
   - the cleanup callback must be safe to run on the RHI thread.

## 1. Prerequisite fixes (do these first; they apply to every mode)

1. **Thread-time capture.**
   - Find out why `RenderThreadMs`/`RHIThreadMs` read about 0 in the Phase 2 run (PIE, Baseline). Fix it so all modes report correct values in both PIE and standalone `-game`.
   - Validate against `stat unit` in a short side-by-side run, and document the result.
   - If the engine globals can't be made reliable, measure the render and RHI thread busy time directly instead, and document how.
2. **Checksums off for performance runs.**
   - `RunBenchmark` gets `-Checksums On|Off`, default **Off** for `-Suite`, `-Scenario` and `-Repeat`.
   - The summary header prints the checksum state, and every key-stats block includes `[Key] Checksums = On|Off`.
   - Correctness runs can still turn checksums on.

## 2. New Baseline variant: `BaselineSingleUse`

- Same as Baseline, except each new `FTexture2DMipMap` bulk data gets `BULKDATA_SingleUse` before it is handed to `PlatformData`. That lets `FTexture2DResource` take ownership of the mip memory instead of copying it on the game thread.
- Selected with `-StsMode=BaselineSingleUse` or the `UploadMode` knob.
- Report:
  - the change in `UpdateResourceGTMs`;
  - the retained `PlatformData` memory (it should drop to about 0, because the bulk data moves into the resource and is freed after creation);
  - that nothing breaks. In particular, check what happens if anything reads `PlatformData` later.
- This is a candidate **one-line production patch**, measured separately so the user can decide.

## 3. Regions mode (`-StsMode=Regions`)

### 3.1 Pools
- One pool per resolution. Slot count = in-use + `PoolSlack<Res>` (the same knobs as Phase 2).
- **All slots are created at `CreateTiles`, GPU resource included.**
  - Each slot calls `UpdateResource()` exactly once, with the Variant 0 library payload for its resolution.
  - The initial mips use `BULKDATA_SingleUse`, so creation does not copy on the game thread and `PlatformData` bulk memory is released.
  - Keep the `PlatformData` object, sizes and mip entries so `GetSizeX/Y`, `GetNumMips` and the stats still work. Verify nothing reads the freed bulk data later.
- After initialisation, a slot is marked `Initialized`. Any path in our code that would call `UpdateResource` on an initialised slot must `ensure` and count `UpdateResourceAfterInit`, which must stay 0. `RegenerateTextures` and `RebuildTiles` re-create the pools rather than calling `UpdateResource` on live slots.
- Reuse policy: **the same per-resolution FIFO as Phase 2**, with the same reuse-age, `UnsafeReuses` and `PoolExhausted` accounting. That way the only difference from Baseline is the upload path.
- Pre-allocating every slot raises VRAM up front (default slack: about 3.3 GB of slots). The HUD and summary show the pool VRAM per resolution.

### 3.2 Uploader interface
- Add an uploader interface (for example `ISTSTextureUploader`). It takes a slot texture, the payload's buffer plus mip ranges, and a completion callback that is safe on any thread.
- Implement `FSTSRegionsUploader` now. Phase 4 adds a batched custom uploader behind the same interface.

### 3.3 Upload of one response
1. Released response (game thread). The Phase 1 checksum runs only if enabled.
2. Take the bottom slot from the target resolution's FIFO, and record its reuse age.
3. For each mip, call `UpdateTextureRegions(MipIndex, 1, Region, SrcPitch, SrcBpp, SrcData, Cleanup)`, where:
   - `Region` is heap-allocated: the whole mip from (0,0), with width and height **padded up to a multiple of 4** (so tail mips are 4×4);
   - `SrcPitch = max(1, w/4) * 8`;
   - `SrcBpp` is unused because offsets are 0 (pass 8 and document it);
   - `SrcData` points at that mip's range inside the response buffer.
4. **Buffer lifetime:**
   - The response buffer is owned by a shared holder that every mip's cleanup releases. The buffer is freed or returned to its pool when the **last** cleanup runs, on the RHI thread.
   - Each cleanup also frees its region.
   - When the last cleanup runs, the slot is marked "upload complete" (a thread-safe flag or counter read on the game thread). Phase 5 will use it for event-driven reuse; Phase 3 only records the time from upload issued to upload complete.
5. Set the slot on the tile's MID in the same frame (render commands run in order, before this frame's rendering). The tile's previous slot goes on the top of its FIFO. Update the visual state.

### 3.4 Payload buffer pool
- A per-resolution pool of payload buffers (`FSTSPayloadBuffer` is already swappable). Server workers take a buffer from the pool instead of allocating, and the last upload cleanup returns it from the RHI thread. The pool must be thread-safe.
- `PayloadBufferPool<Res>` knobs are sized to cover in-flight responses plus queued uploads.
- If a pool is empty: allocate a fresh buffer, count `BufferPoolMisses`, and never block a worker.
- Baseline modes keep their per-response allocation (production behaviour).

## 4. Measurements

**Per-texture render-thread upload time (`RTUploadMs`):** render-thread timestamp markers before the first mip's update and after the last, the same technique as `RTCreateMs`. Reported by resolution.

**Cost model (`bPerMipTiming` knob, default off):**
- When on, markers go around **each mip's update**, and each sample (bytes, render-thread µs) is recorded.
- The summary fits `time = FixedCost + Bytes / Throughput` by least squares, separately for the lock path and the staging path if both occur. It reports the fixed cost per call (µs), throughput (GB/s), R², and the number of calls per burst frame × fixed cost.
- This number decides whether Phase 4 is worth doing.
- Per-mip markers add commands, so performance runs keep this off.

**Game-thread cost:** `UploadGTMs`, the time of all `UpdateTextureRegions` calls per frame.

**Upload completion latency:** from upload issued to last cleanup, by resolution (average / p99 / maximum).

**CSV (append only):**
- `RTUploadMs`, `UploadGTMs`;
- `UploadsInFlight` (issued, not complete);
- `BufferPoolMisses`, `UpdateResourceAfterInit`;
- `PoolVramMB`.

Regions mode leaves the Baseline-only columns at 0 (`BuildMs`, `RTCreateMs` and the like). In Regions mode, `Uploads` and `UploadMB` count `UpdateTextureRegions` uploads.

**Summary:** the mode, the checksum state, the cost-model fit (if enabled), `RTUploadMs` by resolution, upload completion latency, pool VRAM, buffer pool stats, and the existing reuse/consistency stats.

**Trace:** scopes for slot acquire, upload issue, and the per-mip calls (only when `bPerMipTiming` is on). Bookmarks for the first upload of each burst.

## 5. Correctness tools

**`sts.Check.ReadbackMips [Resolution]`** (plus a button):
- For a few in-use slots per resolution (or the one given), read back **every mip** from the GPU. Use a debug-only readback (a render-thread lock with read-only access, or a copy to a staging texture); it may stall, which is acceptable.
- Compare each mip's bytes with the payload that slot should hold (by tile, resolution and variant), and log any mismatch per mip.
- **Mandatory for the 2×2 and 1×1 tail mips.**

The existing consistency check keeps working, and is also run in Regions mode.

## 6. Benchmark runner

- `-StsMode` accepts `Baseline | BaselineSingleUse | Regions`.
- `-Suite` takes a list: `-StsMode Baseline,BaselineSingleUse,Regions`. Each mode runs the full suite (uncapped and capped at 60 fps, 3 repeats each).
- The suite writes **one combined comparison table** across modes, with deltas against Baseline, plus the run-to-run spread per mode.
- Standalone runs use the benchmark render profile. The summary must show it as **APPLIED**; PIE results are labelled "PIE (not comparable)".

## 7. Acceptance (user validation)

1. `.\BuildGame.cmd` is clean. The Phase 0–2 acceptance lists still pass in their modes.
2. **Measurement fix:** `RenderThreadMs`/`RHIThreadMs` agree with `stat unit` (within reason) in PIE and standalone, for every mode.
3. **Correctness (Regions):**
   - visuals correct;
   - consistency mismatches 0;
   - `ReadbackMips` mismatches 0 for every resolution and every mip, including 2×2 and 1×1;
   - checksum failures 0 in a checksums-on correctness run.
4. **Invariants (Regions):**
   - `UpdateResourceAfterInit` = 0;
   - `UnsafeReuses` = 0 and `PoolExhausted` = 0 at default slack;
   - `BufferPoolMisses` reported (0 at default sizing);
   - RHI texture / GPU local memory **flat** during bursts (no allocation spikes).
5. **BaselineSingleUse:** works, and its game-thread `UpdateResource` change and retained-memory change are reported.
6. **Comparison:** `RunBenchmark -Suite -StsMode Baseline,BaselineSingleUse,Regions` runs unattended (standalone, benchmark profile applied, checksums off). It produces the comparison table with deltas and the noise band.
7. **Cost model:** one Burst run with `bPerMipTiming` on reports the fixed cost per call, the throughput and R², and the projected fixed cost per burst frame.
8. **Report:**
   - files changed;
   - flow description;
   - knobs and commands;
   - CSV/summary definitions;
   - the thread-time root cause and its fix;
   - the comparison table;
   - the cost-model result;
   - known limitations.

**Then stop.** Phase 4 starts only after the user signs off.

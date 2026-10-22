# Phase 2 – Baseline: the Current Production Path (specification)

Parent plan: [`PrototypePlan.md`](PrototypePlan.md). Builds on Phase 0 ([`Phase0_Harness.md`](Phase0_Harness.md)) and Phase 1 ([`Phase1_TerrainServer.md`](Phase1_TerrainServer.md)).

Phase 2 reproduces the **product's current texture update path** as faithfully as possible. It drives it with the Phase 1 server, and records the "before" numbers that every later phase is compared against. **Matching production matters more than performance or elegance here.** Do not optimize the baseline.

## 1. Production path being reproduced (confirmed by the user)

1. There is **one circular FIFO queue of `UTexture2D` objects per resolution** (512, 1k, 2k, 4k). Released objects go on the top; objects are taken from the bottom.
2. A **background thread** builds the mips for each received payload:
   - it allocates a new `FTexture2DMipMap` per mip;
   - it does `BulkData.Lock(LOCK_READ_WRITE)` → `Realloc(size)` → memcpy from the server buffer → `Unlock`.
3. On the **game thread**, the next object is taken from the bottom of the target resolution's queue:
   - its old `PlatformData->Mips` are emptied (which frees the old mip memory);
   - the new mips are handed over with `Mips.Add(pointer)`. This is an ownership transfer, not a copy;
   - then `UpdateResource()` is called.
4. The new texture is **set on the tile's MID right away** (`SetTextureParameterValue`), and the tile's previous texture goes on the top of its own resolution's queue.
5. There is **no frame wait**. Safety comes only from the pool being big enough that a released object reaches the bottom of its queue long after it was released (at least the number of frames the GPU is behind).
6. `PlatformData` is kept on every object, in use or free. That retained CPU memory is part of the baseline cost.

## 2. Upload mode selection

- A new enum `ESTSUploadMode`: `LogicalOnly` (Phase 1 behaviour: responses only update logical state) and `Baseline`. Later phases add `Regions`, `CustomUpdate` and `CustomLock`.
- A Details knob `UploadMode` [Baseline]. Override it on the command line with `-StsMode=LogicalOnly|Baseline`, which takes precedence.
- The mode is fixed while tiles exist. Changing it in the editor takes effect on `RebuildTiles`.
- The summary header records the mode.

## 3. Pools

- For each resolution: the objects in use (the tiles' current textures) plus **`PoolSlack<Res>`** spare objects. Default slack = 100% of the tiles at that resolution, with a minimum of 32. These are separate knobs per resolution.
- Spare objects are created at `CreateTiles` as `UTexture2D` UObjects with the correct size and format, but **without** a GPU resource. They get one on first use, through the normal path. This matches production ("objects are requested from the pool, and textures created, the first time").
- The tile textures from initial Phase 0 creation become pool members (in use).
- `DestroyTiles` tears the pools down, and the existing forced GC applies.
- The HUD and summary show the free/in-use counts per resolution, and the CPU memory retained in `PlatformData` per resolution.

## 4. Baseline consumer

**Released response (game thread):**
- Run the Phase 1 checksum, if enabled.
- Move the response buffer into a build job for the background mip builder.

**Mip builder:**
- `BaselineBuilderThreads` [1] dedicated thread(s), to mimic production.
- It builds the `FTexture2DMipMap` set (§1.2) from the payload's mip offsets, and frees the response buffer.
- It pushes the finished job to a game-thread completion queue.
- No UObject access on the builder thread.

**Apply (game thread, each frame, all finished jobs):**
1. Discard jobs with a stale epoch and count them.
2. Take the bottom object from the target resolution's queue.
   - If the queue is empty, count `PoolExhausted` and leave the job queued for the next frame. Do not allocate new objects; that would hide the problem.
3. Record the **reuse age**: frames since this object was released. If it is below `SafeReuseFrames` [3], count `UnsafeReuses`.
4. Replace `PlatformData->Mips` (§1.3), then call `UpdateResource()`. Wrap that call in **render-thread timestamp markers** (§5).
5. Call `SetTextureParameterValue` on the tile's MID. The tile's old texture goes on the top of its queue, with the current frame recorded as its release frame.
6. Update the tile's **visual** resolution and variant (add `VisualVariant` to `FSTSTile`), and clear `PendingRequestId`.

**Tile destruction:** destroying tiles with build jobs in flight must be safe. Stale jobs are dropped and their mips freed.

## 5. Measurements

**Render-thread texture creation time:**
- Immediately before each `UpdateResource`, enqueue a render command that records a timestamp. Immediately after, enqueue one that computes the delta.
- Accumulate these deltas per frame (thread-safe), and read them back on the game thread. They arrive about one frame late; document this.
- Explain in the report which part of the D3D12 resource creation and upload this captures on the render thread, and what remains on the RHI thread (visible in `RHIThreadMs`). Check this against the 5.6 source.

**CSV:**
- Fill the reserved columns: `Uploads` (applied this frame) and `UploadMB`. `CacheHits` stays 0.
- **Append** these columns:
  - `BuildMs` (sum of builder time for jobs finished this frame), `BuildQueueDepth`;
  - `ApplyGTMs` (whole apply step), `UpdateResourceGTMs` (only the game-thread call), `RTCreateMs` (from the markers);
  - `Free512`, `Free1k`, `Free2k`, `Free4k`;
  - `MinReuseAgeFrames` (this frame; -1 if none), `UnsafeReuses`, `PoolExhausted`;
  - `IssueToVisibleAvgMs`, `IssueToVisibleMaxMs`, `ReleaseToVisibleMaxMs`. "Visible" means the frame the MID was set; the first draw is about one frame later. Document this.
  - `GpuLocalMemMB`: GPU local memory in use, if 5.6 exposes it through the RHI (for example DXGI video memory info in the D3D12 RHI stats). Otherwise leave it out and say so.

**Summary additions:**
- per-resolution pool sizes and slack;
- the distribution of reuse age (min / p1 / p50);
- total unsafe reuses and pool-exhausted events;
- issue → visible latency: average, p50, p99, maximum;
- RTCreateMs per texture by resolution (average and maximum);
- builder thread time per texture by resolution.

**Trace:** scopes for build, apply, `PlatformData` swap and `UpdateResource`. Bookmarks for the first apply of each burst.

## 6. Consistency check

- A button `CheckConsistency` and a console command `sts.Check.Consistency`. For every tile it checks:
  - visual resolution and variant equal the logical ones;
  - the texture's actual size equals the visual resolution;
  - the MID's texture parameter is the tile's texture.
- It logs the mismatch count plus the first few mismatches.
- New scenario step `CheckConsistency`, appended after `WaitForIdle` in every preset.

## 7. Benchmark runner extensions

- `RunBenchmark -StsMode <Mode>` passes the mode through.
- `RunBenchmark -Repeat N` runs the same configuration N times in a row, and tags each run's file names with the run index.
- `RunBenchmark -Suite`: for the given mode, runs SteadySpread, SpreadSweep, Burst and Oscillation, each **uncapped and capped at 60 fps**, **3 repeats each**. Soak is excluded unless `-IncludeSoak` is given.
  - The suite then writes **one aggregate table** (CSV plus Markdown) under `Saved/SimTexStrm/Suites/`: one row per scenario × cap × run, with the key stats from §8.
  - It also writes the **run-to-run spread** of max and p99.9 frame time per scenario.
- Existing switches (`-Sequence`, `-Scenario`, `-Trace`) keep working.

## 8. Key stats (the aggregate table columns, in priority order)

1. Frame time: max, p99.9, p99, frames > 16.67 ms (missed frames in capped runs).
2. RenderThreadMs and RHIThreadMs: max and p99.9. RTCreateMs per texture (average / maximum) by resolution.
3. GpuMs: max and p99.9.
4. RHI texture memory / GPU local memory: peak and end-of-run. Process RAM: peak and end-of-run.
5. Issue → visible latency: p99 and maximum.
6. ApplyGTMs and UpdateResourceGTMs: max.
7. UnsafeReuses, PoolExhausted, ChecksumFailures, consistency mismatches. **All must be 0.**
8. Harness overhead reference: `ConsumerMs` from Phase 1.

## 9. Acceptance (user validation)

1. `.\BuildGame.cmd` is clean, and the Phase 0 and Phase 1 acceptance lists still pass in `LogicalOnly` mode.
2. **Faithfulness.** The report describes the implemented flow step by step against §1, and the **user signs off that it matches production**.
3. **Visuals.** Every update shows the right colour, stripe and resolution, with correct mips. No black, stale or corrupted tiles. Popping is expected.
4. **Consistency.** Zero mismatches after every preset.
5. **Safety:**
   - UnsafeReuses = 0 and PoolExhausted = 0 at the default slack in every preset;
   - **negative test:** at very small slack (for example 2), unsafe reuses and/or pool-exhausted events are reported, and the run does not crash.
6. **No leaks.** A 10-minute soak (custom or shortened preset) shows process RAM and RHI texture memory returning to the same level between bursts.
7. **Suite.** `RunBenchmark -Suite -StsMode Baseline` runs unattended and writes the aggregate table and the run-to-run spread.
8. **Report.** It includes the baseline results table, the noise band (run-to-run spread), the reuse-age distribution, and RTCreateMs per resolution.

**Then stop.** Phase 3 starts only after the user signs off.

# Phase 0 – Static Harness (specification)

Parent plan: [`PrototypePlan.md`](PrototypePlan.md). Phase 0 has **no texture swapping**. It builds a controllable, measurable static scene that every later phase runs on.

## 1. Deliverables

1. **Module setup.** Add `RenderCore`, `RHI`, `MeshDescription` and `StaticMeshDescription` (plus anything else required) to `SimTextureStrm.Build.cs`.
2. **`ASTSTestbench`**, a `Blueprintable`, placeable actor that owns everything below. All knobs are `EditAnywhere`/`BlueprintReadWrite` properties, grouped under `STS|...` categories in the Details panel.
3. **Runtime triangle meshes.** Two `UStaticMesh` assets built at runtime from `FMeshDescription`: a lower-left triangle and an upper-right triangle.
4. **Tile field.** A grid of cells, each split into two triangle tiles. Each tile has its own `UStaticMeshComponent`, its own `UMaterialInstanceDynamic` and its own `UTexture2D`.
5. **Procedural DXT1 generator** with full mip chains and configurable look.
6. **Benchmark recorder** that writes a per-frame CSV plus a percentile summary.
7. **On-screen HUD.**
8. **Console commands** that mirror the Details-panel buttons, because the Details panel isn't available in standalone `-game` runs.
9. **`RunBenchmark.cmd` + `RunBenchmark.ps1`**, which launch standalone `-game` with the benchmark render profile.
10. **Phase 0 report** for the user (see §11).

Not in Phase 0: the synthetic server, texture updates, pools, upload modes, and any content assets (the user creates those; see §10).

## 2. `ASTSTestbench` knobs (Details panel)

Defaults are shown in brackets. The implementer can add knobs but should not remove these.

### STS|Tiles
- `bCreateTilesOnBeginPlay` [false]. When false, the scene starts empty, so the user can record "before tiles".
- `GridCellsX` [25], `GridCellsY` [20]. Tiles = 2 × X × Y (default 1000). Clamp to a sane maximum (for example 10000 tiles) and warn.
- `CellSizeCm` [50000] (500 m cells).
- `TilesCreatedPerFrame` [0]. 0 creates all tiles in one frame (the worst-case hitch); N > 0 time-slices creation.
- `TileMaterial` (`UMaterialInterface*`). Must expose a texture parameter named by `TextureParameterName` [`TileTexture`]. If it is unset or the parameter is missing, log a warning, show it on the HUD, and fall back to the engine default material.
- `bTilesCastShadows` [false].
- `bForceGarbageCollectionOnDestroy` [true]. After destroying tiles, force a GC so memory release is measurable.

### STS|Resolution
- Resolution is assigned by **distance rank from the grid centre**: the nearest `Count4k` tiles get 4k, the next `Count2k` get 2k, then `Count1k` get 1k, then `Count512` get 512.
- `Count4k` [100], `Count2k` [200], `Count1k` [300], `Count512` [400].
- `RemainderResolution` [512] applies to tiles beyond the sum of the counts. If the counts exceed the tile total, fill from 4k down and warn.

### STS|Texture Look
- `Color512` [red], `Color1k` [green], `Color2k` [blue], `Color4k` [yellow], all as `FColor`.
- `BorderColor` [near-black], `BorderWidthFraction` [0.004] of the texture width. At least 1 texel at every mip, so borders survive distance.
- `bDrawDiagonal` [true]. Draws the anti-diagonal (u + v = 1), so each triangle shows all three of its edges.
- `MipCoding` enum: `None`, `Brightness` (default), `Checker`.
  - `Brightness` alternates the base-colour brightness per mip, for example 100% / 55%.
  - `Checker` adds a per-mip checker frequency.
- `MipBrightnessLow` [0.55].
- `bSRGB` [true].
- Each mip is **authored independently** (not downsampled), so mip selection and tail-mip correctness are visible.

### STS|Camera
- `CameraPreset` enum: `Oblique` (default), `TopDown`, `Custom`.
- `ObliquePitchDeg` [-12], `FieldOfViewDeg` [60], `bAutoFrame` [true]. Auto-framing places the camera so the whole grid is on screen.
- `bUseTestbenchCameraOnBeginPlay` [true]. Sets the player's view target to the testbench's `UCameraComponent`. `Custom` uses the component's transform as placed by the user.

### STS|Recorder
- `RecordName` [`Run`]. Output goes to `Saved/SimTexStrm/<RecordName>_<yyyyMMdd_HHmmss>.csv` and `_summary.txt`.
- `RecordDurationSec` [0 = manual stop].
- `bRecordOnBeginPlay` [false], `RecordStartDelaySec` [3] (warm-up before recording starts).

### STS|Sequence (automated before/after capture)
- `bRunPhase0SequenceOnBeginPlay` [false]. When true, runs these steps:
  1. warm up for `SeqWarmupSec` [5];
  2. record `Empty` for `SeqRecordSec` [20];
  3. create tiles;
  4. settle for `SeqSettleSec` [5];
  5. record `Tiles` for `SeqRecordSec`;
  6. optionally destroy tiles, settle, and record `AfterDestroy` (`bSeqIncludeDestroy` [true]);
  7. optionally quit (`bSeqQuitWhenDone` [false]; set true by `RunBenchmark`).
- The tile-creation frame is recorded separately: its duration is logged, and it appears in the CSV if a recording is active.

### STS|HUD
- `bShowHud` [true].

### Buttons (`UFUNCTION(CallInEditor)`, usable on the placed instance during PIE)
- `CreateTiles`, `DestroyTiles`, `RebuildTiles` (destroy, then create with the current knobs), `RegenerateTextures` (rebuild the payload library and re-create textures).
- `StartRecording`, `StopRecording`, `RunPhase0Sequence`.

## 3. Console commands (standalone-friendly)

Each command operates on the first `ASTSTestbench` in the world, and logs an error if there is none.

- `sts.Tiles.Create`, `sts.Tiles.Destroy`, `sts.Tiles.Rebuild`
- `sts.Record.Start [Name] [DurationSec]`, `sts.Record.Stop`
- `sts.Seq.Phase0`
- `sts.Hud 0|1`

## 4. Geometry

- Cell (x, y) is laid out on the XY plane, centred on the actor. Z is up.
- Lower-left triangle: local vertices (0,0), (S,0), (0,S) with UVs (0,0), (1,0), (0,1).
- Upper-right triangle: local vertices (S,0), (S,S), (0,S) with UVs (1,0), (1,1), (0,1).
- Both hypotenuses lie on u + v = 1, so **one texture design (square border plus anti-diagonal) works for both orientations**.
- Build each mesh once and share it across all tiles. One material slot, +Z normals, and a correct winding for front faces seen from above.
- Components: no collision, `CastShadow` from the knob, no tick.

## 5. Procedural DXT1 generator

- Output: a mip chain from W×H down to 1×1, as `TArray` byte buffers per mip. DXT1 uses 8 bytes per 4×4 block. Mips smaller than 4×4 are still one block.
- Blocks use 4-colour mode (`color0 > color1` in RGB565). Pick two endpoints per block from the texels it contains (base colour and border colour). Indices are per texel.
- The generator runs on worker threads (`ParallelFor` over blocks or rows), with a trace scope around it.
- **Payload library:** one generated chain per resolution for the current look settings, cached and regenerated only when look settings change. All tiles of the same resolution share the source bytes, but each tile still gets **its own `UTexture2D`**, so the VRAM and CPU footprint match production.
- The generator API must accept a `Variant` integer (unused in Phase 0). Phase 1 will use it for a per-update stripe.

## 6. Per-tile texture creation (the same as the product's creation path)

- `UTexture2D::CreateTransient(W, H, PF_DXT1)`, then fill `PlatformData->Mips` with **all** mips (add `FTexture2DMipMap` entries and copy the bulk data).
- Texture settings: `SRGB` from the knob, `LODGroup = TEXTUREGROUP_World` (so world sampler / anisotropic settings apply), `NeverStream = true`, default filter.
- Call `UpdateResource()` once. Keep `PlatformData` (production keeps it; the CPU memory footprint is part of the baseline).
- Then set it on the tile's MID through `SetTextureParameterValue(TextureParameterName, Tex)`.
- Hold strong references (`UPROPERTY`) to meshes, components, MIDs and textures. `DestroyTiles` destroys the components and clears the references (plus an optional forced GC).

## 7. Benchmark recorder

**Per-frame CSV columns:**
- `Frame`, `TimeSec`, `FrameMs`
- `GameThreadMs`, `RenderThreadMs`, `RHIThreadMs`, `GpuMs`
- `TilesAlive`, `TexMB_512`, `TexMB_1k`, `TexMB_2k`, `TexMB_4k` (calculated from the tiles alive)
- `ProcessPhysMB`
- `RHITextureMB` (if available from RHI stats)
- **Reserved for later phases, written as 0 now:** `Uploads`, `UploadMB`, `CacheHits`, `PendingRequests`

Use engine thread timings (`GGameThreadTime`, `GRenderThreadTime`, `GRHIThreadTime`, `RHIGetGPUFrameCycles`, or whatever is correct in 5.6). Document which source each column comes from and any frame latency (GPU time lags by 1–2 frames).

**Summary** (`_summary.txt` plus the log), for each timing column:
- sample count, average, p50, p99, p99.9, maximum;
- the number of frames > 16.67 ms and > 33.33 ms;
- the run name, the effective render settings (see §9), resolution, VSync/`t.MaxFPS`, tile counts and texture MB per resolution.

**Trace:** `TRACE_CPUPROFILER_EVENT_SCOPE` around generation, tile creation, texture creation and `UpdateResource`. `TRACE_BOOKMARK` on record start/stop and on tiles created/destroyed.

## 8. HUD

Drawn with a debug draw service or a canvas, on when `bShowHud` is set:

- FPS and the four thread/GPU times;
- tiles alive, and tile count plus MB per resolution;
- process RAM;
- recorder state (name, elapsed time);
- sequence step;
- material warnings.

## 9. Benchmark launch (`RunBenchmark.cmd` + `RunBenchmark.ps1`)

- Follow the existing `LaunchProject.cmd` / `.ps1` pattern: `ProcessStartInfo` with an explicit argument string, for paths with spaces.
- Launch `UnrealEditor.exe "<uproject>" /Game/StartupMap -game` with these parameters:
  - `-Mode Uncapped|Capped60` (default `Uncapped`).
    - `Uncapped` uses `-ExecCmds="r.VSync 0,t.MaxFPS 0"`.
    - `Capped60` uses `"r.VSync 0,t.MaxFPS 60"`.
  - `-ResX`/`-ResY` (default 3840×2160, windowed, `-ForceRes`).
  - `-Trace` switch: adds `-trace=cpu,gpu,frame,bookmark,memory -statnamedevents` and a trace file under `Saved/SimTexStrm/`.
  - `-Sequence` switch: runs the Phase 0 sequence and quits when done.
  - Passthrough of extra arguments. `-StsMode=...` is reserved for Phase 2+ and is ignored for now.
- **Benchmark render profile:** Lumen off (`r.DynamicGlobalIlluminationMethod=0`, `r.ReflectionMethod=0` or `2`), ray tracing off (`r.RayTracing=False`, startup-only, so it needs an ini override on the command line), and Virtual Shadow Maps off (`r.Shadow.Virtual.Enable=0`).
- At `BeginPlay`, the testbench **logs the effective values** of these CVars, and the summary records them. This verifies that the overrides actually took effect. If a command-line ini override doesn't apply in 5.6, pick a working mechanism and document it.
- Do not modify the existing scripts.

## 10. User steps (in the editor, after the code builds)

1. **Level.** File → New Level → Empty Level. Save it as `/Game/StartupMap`.
   - Add a Directional Light and a Sky Light, so the Default Lit material is visible.
   - Optionally add an Exponential Height Fog (or leave it out, for less noise).
2. **Material** `/Game/SimTexStrm/M_STS_Tile`:
   - Material Domain: Surface. Blend Mode: Opaque. Shading Model: Default Lit. Two Sided: off.
   - A **TextureSampleParameter2D** named `TileTexture`: Sampler Type Color, **Sampler Source "Shared: Wrap"** (world sampler with anisotropic filtering), and any engine texture as the default.
   - `RGB → Base Color`, constant `Roughness 0.8`, constant `Specular 0.2`. Save.
3. **Blueprint.** Create `BP_STSTestbench` (parent `ASTSTestbench`) in `/Game/SimTexStrm/`, set `TileMaterial = M_STS_Tile`, and place one instance in `StartupMap`. Save.
4. **Game mode.** Use a default game mode that doesn't spawn anything distracting (the default is fine; the testbench takes the view target).

## 11. Phase 0 acceptance (user validation)

1. **Build.** `.\BuildGame.cmd` succeeds with no warnings in project code.
2. **Toggling.** Starting with no tiles, then `CreateTiles` (button or `sts.Tiles.Create`), gives 1000 triangle tiles forming a seamless skin. `DestroyTiles` removes them, and `RebuildTiles` applies changed knobs (grid size, counts, colours, borders, mip coding).
3. **Visuals:**
   - resolution colours in concentric distance-rank rings;
   - borders visible on all three edges of every triangle;
   - mip coding changes plausibly with distance and grazing angle;
   - no shimmering from missing mips.
4. **Sequence.** `RunBenchmark.cmd -Sequence` produces `Empty`, `Tiles` and `AfterDestroy` CSVs plus summaries, and the summaries show the render profile applied.
5. **Memory.** RAM and texture MB rise by about the expected ~1.9 GB (VRAM) plus the retained `PlatformData` copies on the CPU, and fall back after destruction.
6. **Hand-off.** The implementer reports:
   - the files added or changed;
   - how to use each knob and command;
   - the data source for each CSV column;
   - known limitations;
   - the measured tile-creation hitch duration.

**Then stop.** Phase 1 starts only after the user signs off.

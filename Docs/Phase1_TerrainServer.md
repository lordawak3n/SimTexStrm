# Phase 1 – Synthetic Terrain Server and Update Knobs (specification)

Parent plan: [`PrototypePlan.md`](PrototypePlan.md). Builds on the Phase 0 harness ([`Phase0_Harness.md`](Phase0_Harness.md)): `ASTSTestbench`, `FSTSDxt1Generator`, `FSTSRecorder`, `STSConsoleCommands.cpp`.

Phase 1 produces a **controllable, deterministic stream of texture update requests and payload deliveries**, which later phases will consume. There is **no upload path** in Phase 1. Payloads are received, validated, accounted for, and released. Tile textures on screen do not change because of requests.

Phase 0 behaviour (tile creation/destruction, Phase 0 sequence, recorder, HUD, `RunBenchmark`) must keep working unchanged.

## 1. Deliverables

1. **`FSTSTerrainServer`**: a worker-thread synthetic server with a pre-generated payload library, modelled latency and link bandwidth, deterministic delivery, and checksums.
2. **Variant stripe** in `FSTSDxt1Generator` (the `Variant` parameter becomes active).
3. **Per-tile logical state**, so update patterns (step up/down) progress realistically even though the visuals don't change yet.
4. **Update triggers:** burst, spread, patterns, and target selection, all deterministic from a seed.
5. **Scenario runner:** a data-driven list of steps, editable in the Details panel, with presets.
6. **Recorder, HUD and console extensions.**
7. **`RunBenchmark` extension:** a `-Scenario` switch.
8. **Phase 1 report** (see §12).

## 2. Payload library

- Generated at startup (and on `RegenerateTextures` or a look change) for every resolution in use × `VariantsPerResolution` [4] variants. It uses the existing `FSTSDxt1Generator` on worker threads.
- Each entry is a **contiguous** DXT1 buffer with all mips (base level first), plus an offset and size per mip. This layout is what Phase 2 and later need: one buffer per payload.
- Each entry stores a 64-bit checksum (for example `CityHash64`, or any 64-bit hash available in 5.6) computed at generation.
- **Variant stripe:** `Variant = 0` looks exactly like Phase 0. `Variant = k > 0` draws a stripe pattern that encodes k: for example k diagonal bars in a contrasting colour, which must stay visible at every mip down to 4×4. That way a same-resolution content swap is visible once uploads exist.

## 3. `FSTSTerrainServer`

A plain C++ object owned by the testbench. It is not a UObject.

**Request:** `{ RequestId, TileIndex, TileEpoch, Resolution, Variant, IssueFrame, IssueTimeSec, Priority }`.

**Workers:**
- `ServerWorkerCount` [4] workers. Use dedicated threads or UE Tasks; document the choice.
- Each worker takes a request and **memcpys** the library entry into a newly allocated response buffer. This simulates receiving the data over the network into a client buffer.
  - Response: `{ Request, Payload buffer, mip offsets/sizes, checksum, WorkerDoneTimeSec }`.
- Per-response allocation is acceptable in Phase 1. Buffer pooling comes in Phase 3. Keep the buffer type swappable.

**Modelled timing (deterministic, no sleeping):**
- `ReadyTimeSec = max(IssueTimeSec + LatencyMs ± Jitter, LinkFreeAtSec) + Bytes / LinkBandwidthMBps`. This models a single serial link, and `LinkFreeAtSec` advances per response in request order.
- Knobs:
  - `ServerLatencyMs` [20];
  - `ServerLatencyJitterMs` [5], seeded per request ID so it is deterministic;
  - `ServerLinkBandwidthMBps` [0 = unlimited; 1100 ≈ 10GbE];
  - `ServerMaxInFlight` [0 = unlimited].
- Workers push to an MPSC completion queue. On the game thread, the testbench drains it into a **delay line** ordered by `ReadyTimeSec`. Responses are **released** to the consumer only when `Now >= ReadyTimeSec`. If a worker is slower than the model, the response is released when the worker finishes, and that is counted as `LateResponses`.

**Lifetime:**
- Stop and drain cleanly on `EndPlay`.
- On `DestroyTiles` and `RebuildTiles`, bump `TileEpoch`. Responses with a stale epoch are discarded and counted.
- No crashes or leaks when tiles are destroyed while requests are in flight.

## 4. Consumer (Phase 1 only)

Each released response is processed on the game thread:

1. If `bVerifyChecksums` [true] is set, recompute the checksum and compare it. Count mismatches, and log the first few with request details.
2. Update the tile's **logical state** (§5).
3. Account for it in the stats (§9), then free the buffer.

The checksum cost is measured separately (a trace scope plus a HUD/summary figure), so it can be disabled during benchmark runs. There is no texture update and no MID change.

## 5. Per-tile logical state

Add to each tile:

- `LogicalResolution`: initialised from the Phase 0 resolution.
- `LogicalVariant`: initially 0.
- `PendingRequestId`: `INDEX_NONE` when idle.

On release, the logical state takes the response's resolution and variant. A tile with a pending request is **not re-targeted** unless `bAllowRetargetInFlight` [false] is set. The HUD shows logical tile counts per resolution next to the visual (Phase 0) counts.

## 6. Update patterns (`ESTSUpdatePattern`)

- **`SameResolution`**: new content, `Variant = (LogicalVariant + 1) % VariantsPerResolution`, skipping 0 after the first swap if `bSkipVariantZero` [true].
- **`StepUp`** / **`StepDown`**: one power-of-two step from `LogicalResolution`, clamped to 512..4096.
- **`Mixed`**: weights per pattern [Same 1, Up 1, Down 1].
- **`Oscillate`**: the targeted tiles alternate up and down on every trigger (this prepares the Phase 5 cache tests).

`ImpossibleStepPolicy` decides what happens when a step can't be made (for example StepUp on a 4k tile):

- `PickAnotherTile` (default): only eligible tiles are selected.
- `FallbackToSameResolution`.
- `Skip`: the step is counted as skipped.

## 7. Target selection (`ESTSTargetMode`)

- **`Random`**: from all eligible tiles.
- **`RankRange`**: tiles whose distance rank from the grid centre is within `[RankMin, RankMax]` (the same ranking Phase 0 uses for resolution rings).
- **`Patch`**: a contiguous rectangle of cells (`PatchCenterX/Y`, `PatchSizeX/Y`, both triangles of each cell). This simulates a ring shift. `PatchStepX/Y` optionally moves the patch on every trigger, to simulate travel.

Determinism:
- `UpdateSeed` [12345] drives an `FRandomStream`.
- With `bResetSeedPerTrigger` [false] set, every trigger reseeds, so it repeats exactly.
- Each trigger logs the number of tiles selected and a hash of the selected tile index list, so runs can be checked for repeatability.

## 8. Triggers

**Burst** (category `STS|Updates|Burst`):
- `Burst4k`, `Burst2k`, `Burst1k`, `Burst512`: counts by **requested** (target) resolution.
- All requests are issued in **one frame**.
- If there aren't enough eligible tiles, issue what's possible and report requested vs. issued (on the HUD, in the log, and as a summary line).
- Button `TriggerBurst`. Console `sts.Burst [n4k n2k n1k n512]`, with no arguments meaning use the knobs.

**Spread** (category `STS|Updates|Spread`):
- `SpreadRatePerSec` [5], `SpreadDurationSec` [0 = until stopped].
- `SpreadResolutionWeights` [512:1, 1k:1, 2k:1, 4k:1]: the target resolution mix.
- **Timeline-based:** request k is due at `t0 + k / Rate`, and each frame issues every request now due. Across runs, the issue *times* are deterministic. Which frame a request lands in depends on frame timing; document this.
- Buttons `StartSpread`, `StopSpread`. Console `sts.Spread [Rate] [DurationSec]`, `sts.Spread.Stop`.

Both triggers use the current pattern, target mode, and the eligibility rules.

## 9. Recorder, summary, HUD

**CSV:**
- Fill the reserved `PendingRequests` column with the number of requests in flight, including the delay line.
- Keep `Uploads`, `UploadMB` and `CacheHits` at 0 (Phase 2+).
- **Append** these columns after the existing ones (don't reorder): `RequestsIssued`, `ResponsesReleased`, `ResponseMB`, `ResponseLatencyAvgMs`, `ResponseLatencyMaxMs`, `StaleDiscarded`, `ChecksumFailures`, `ConsumerMs` (game-thread time spent draining, validating and releasing).

**Summary additions:**
- totals for requests, responses, MB, stale and checksum failures;
- issue → release latency: average, p50, p99, maximum;
- burst requested vs. issued;
- server knobs, pattern, target mode and seed.

**HUD additions:**
- in flight, issued/s, released/s, MB/s, latency (average/maximum);
- the spread state;
- the scenario step;
- the last burst result;
- logical vs. visual counts per resolution;
- checksum failures, highlighted when non-zero.

**Trace:** scopes for worker copy, drain, checksum and target selection. Bookmarks for burst, spread start/stop and scenario steps.

## 10. Scenario runner

**`TArray<FSTSScenarioStep> ScenarioSteps`** (Details panel, category `STS|Scenario`). Step types:

- `Wait(Sec)`, `WaitForIdle(TimeoutSec)` (until nothing is in flight);
- `CreateTiles`, `DestroyTiles`;
- `RecordStart(Name, DurationSec)`, `RecordStop`;
- `SetPattern`, `SetTargetMode` (with their parameters), `SetSeed`;
- `Burst(n4k, n2k, n1k, n512)`;
- `SpreadStart(Rate, DurationSec)`, `SpreadStop`;
- `Quit`.

Controls:
- `bRunScenarioOnBeginPlay` [false]. Buttons `RunScenario`, `StopScenario`. Console `sts.Scenario.Run`, `sts.Scenario.Stop`.
- `ScenarioPreset` enum plus an `ApplyScenarioPreset` button that fills `ScenarioSteps`:
  - **`SteadySpread`**: create tiles → settle → record 60 s at 5/s mixed → quit.
  - **`SpreadSweep`**: rates 5, 10, 25, 50/s, recording 30 s each.
  - **`Burst`**: settle → record, with three bursts of 20×4k + 40×2k (StepUp then StepDown) 10 s apart.
  - **`Oscillation`**: a fixed patch, oscillating at 2 triggers per second for 60 s.
  - **`Soak`**: 30 min mixed spread at 10/s with periodic bursts.

## 11. `RunBenchmark` extension

- Add `-Scenario <PresetName>` to `RunBenchmark.ps1`/`.cmd`. It applies that preset, runs it on BeginPlay, and quits at the end.
- Keep `-Sequence` (Phase 0) working.
- Every summary records the effective render profile, as in Phase 0.

## 12. Acceptance (user validation)

1. `.\BuildGame.cmd` succeeds with no warnings in project code. Phase 0 acceptance still passes.
2. **Burst:** StepUp burst 20×4k + 40×2k with the default tiles:
   - exactly 20 + 40 requests are issued in one frame (or a shortfall is reported correctly);
   - releases arrive after the modelled latency;
   - the CSV, HUD and summary agree.
3. **Repeatability:** the same seed with `bResetSeedPerTrigger` gives the same selection hash across runs.
4. **Spread:** 10/s for 10 s gives 100 ± 1 requests with timeline-accurate issue times.
5. **Bandwidth model:** with `ServerLinkBandwidthMBps = 1100`, the release spacing of a 4k burst matches bytes divided by bandwidth.
6. **Checksums:** zero failures in every preset.
7. **Variant stripe:** using `RegenerateTextures` with a debug variant knob (`DebugPreviewVariant`, which applies to all tiles through the **Phase 0 creation path only**), the stripe for each variant is visible at all resolutions and mips.
8. **Lifetime:** `DestroyTiles`/`RebuildTiles` while bursts are in flight causes no crash, and stale responses are counted.
9. **Overhead:** two recordings at the same settings, one with the server idle and one with a 50/s spread, show `ConsumerMs` and the game-thread time increase. The report must state how much the harness adds to the game thread with checksums on and with them off, so later phases can separate harness cost from upload cost.
10. **Scenario:** `RunBenchmark.cmd -Scenario Burst` runs unattended, writes its CSVs and summaries, and quits.
11. **Hand-off:** the implementer reports the files changed, every new knob and command, the CSV column sources, the deviations, and the measured harness overhead.

**Then stop.** Phase 2 starts only after the user signs off.

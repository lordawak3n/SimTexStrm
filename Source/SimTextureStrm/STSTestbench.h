// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Math/RandomStream.h"
#include "STSDxt1Generator.h"
#include "STSRecorder.h"
#include "STSTerrainServer.h"
#include "STSTypes.h"
#include "STSTestbench.generated.h"

class APlayerController;
class UCameraComponent;
class UCanvas;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UTexture2D;

/** One triangle tile: its own component, material instance and texture. */
USTRUCT()
struct FSTSTile
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> Component;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Material;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Texture;

	/** The visual (Phase 0) resolution of the texture that is actually on the tile. */
	int32 Resolution = 0;

	int32 CellX = 0;
	int32 CellY = 0;

	/** Distance rank from the grid centre; the ranking that assigns the resolution rings. */
	int32 Rank = 0;

	// Logical state: what the tile would be after the responses released so far. Visuals do not follow it in Phase 1.
	int32 LogicalResolution = 0;
	int32 LogicalVariant = 0;
	int64 PendingRequestId = INDEX_NONE;
};

/** Where a tile will be created; resolved up front so time-sliced creation is deterministic. */
struct FSTSTilePlan
{
	int32 CellX = 0;
	int32 CellY = 0;
	bool bUpper = false;
	int32 Resolution = 512;
	int32 Rank = 0;
	FVector CellOrigin = FVector::ZeroVector;
};

enum class ESTSSequenceStep : uint8
{
	Idle,
	Warmup,
	RecordEmpty,
	WaitForCreate,
	SettleTiles,
	RecordTiles,
	SettleDestroy,
	RecordAfterDestroy
};

/**
 * Phase 0 static harness: a grid of procedural triangle tiles with procedural DXT1 textures,
 * a benchmark recorder, a HUD and console commands (see STSConsoleCommands.cpp).
 * Phase 1 adds the synthetic terrain server, update triggers and the scenario runner
 * (STSTestbenchUpdates.cpp, STSTestbenchScenario.cpp). Responses are validated and accounted for; no texture is updated.
 */
UCLASS(Blueprintable)
class SIMTEXTURESTRM_API ASTSTestbench : public AActor
{
	GENERATED_BODY()

public:
	ASTSTestbench();

	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void OnConstruction(const FTransform& Transform) override;
	//~ End AActor Interface

	// --- Buttons (also reachable through the sts.* console commands) ---

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "STS|Tiles")
	void CreateTiles();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "STS|Tiles")
	void DestroyTiles();

	/** Destroys, then creates again with the current knobs. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "STS|Tiles")
	void RebuildTiles();

	/** Rebuilds the payload library and re-creates the texture of every live tile. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "STS|Tiles")
	void RegenerateTextures();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "STS|Recorder")
	void StartRecording();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "STS|Recorder")
	void StopRecording();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "STS|Sequence")
	void RunPhase0Sequence();

	/** Issues the Burst4k/2k/1k/512 knob counts in one frame. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "STS|Updates|Burst")
	void TriggerBurst();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "STS|Updates|Spread")
	void StartSpread();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "STS|Updates|Spread")
	void StopSpread();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "STS|Scenario")
	void RunScenario();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "STS|Scenario")
	void StopScenario();

	/** Replaces ScenarioSteps with the steps of ScenarioPreset (Custom leaves them untouched). */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "STS|Scenario")
	void ApplyScenarioPreset();

	void StartRecordingNamed(const FString& Name, double DurationSec);
	void SetHudVisible(bool bVisible) { bShowHud = bVisible; }
	bool IsHudVisible() const { return bShowHud; }

	void TriggerBurstCounts(int32 Requested4k, int32 Requested2k, int32 Requested1k, int32 Requested512);
	void StartSpreadRate(double RatePerSec, double DurationSec);
	void SetPatternByName(const FString& Name);
	void SetScenarioPresetByName(const FString& Name);
	void SetChecksumsEnabled(bool bEnabled) { bVerifyChecksums = bEnabled; }
	bool AreChecksumsEnabled() const { return bVerifyChecksums; }

	// --- STS|Tiles ---

	/** When false the scene starts empty, so "before tiles" can be recorded. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Tiles")
	bool bCreateTilesOnBeginPlay = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Tiles", meta = (ClampMin = "1"))
	int32 GridCellsX = 25;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Tiles", meta = (ClampMin = "1"))
	int32 GridCellsY = 20;

	/** Cell edge length in cm. Each cell holds two triangle tiles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Tiles", meta = (ClampMin = "1.0"))
	float CellSizeCm = 50000.0f;

	/** 0 creates every tile in one frame (the worst-case hitch); N > 0 creates N tiles per frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Tiles", meta = (ClampMin = "0"))
	int32 TilesCreatedPerFrame = 0;

	/** Must expose a texture parameter named TextureParameterName; otherwise the engine default material is used. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Tiles")
	TObjectPtr<UMaterialInterface> TileMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Tiles")
	FName TextureParameterName = TEXT("TileTexture");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Tiles")
	bool bTilesCastShadows = false;

	/** Mobility of the tile components. Static gives cached mesh draw commands; the actor root is static so static tiles can attach to it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Tiles")
	TEnumAsByte<EComponentMobility::Type> TileMobility = EComponentMobility::Static;

	/** Forces a full garbage collection after destroying tiles, so the memory release is measurable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Tiles")
	bool bForceGarbageCollectionOnDestroy = true;

	// --- STS|Resolution ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Resolution", meta = (ClampMin = "0"))
	int32 Count4k = 100;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Resolution", meta = (ClampMin = "0"))
	int32 Count2k = 200;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Resolution", meta = (ClampMin = "0"))
	int32 Count1k = 300;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Resolution", meta = (ClampMin = "0"))
	int32 Count512 = 400;

	/** Resolution of the tiles beyond the sum of the counts. Must be 512, 1024, 2048 or 4096. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Resolution")
	int32 RemainderResolution = 512;

	// --- STS|Texture Look ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Texture Look")
	FColor Color512 = FColor::Red;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Texture Look")
	FColor Color1k = FColor::Green;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Texture Look")
	FColor Color2k = FColor::Blue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Texture Look")
	FColor Color4k = FColor::Yellow;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Texture Look")
	FColor BorderColor = FColor(8, 8, 8, 255);

	/** Border width as a fraction of the texture width; at least one texel at every mip of 4x4 or larger. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Texture Look", meta = (ClampMin = "0.0", ClampMax = "0.25"))
	float BorderWidthFraction = 0.004f;

	/** Draws the anti-diagonal (u + v = 1) so each triangle shows all three edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Texture Look")
	bool bDrawDiagonal = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Texture Look")
	ESTSMipCoding MipCoding = ESTSMipCoding::Brightness;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Texture Look", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MipBrightnessLow = 0.55f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Texture Look")
	bool bSRGB = true;

	/** Phase 0 creation path only: the variant stripe drawn on every tile texture the next time textures are created (RegenerateTextures or CreateTiles). 0 is the plain look. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Texture Look", meta = (ClampMin = "0", ClampMax = "15"))
	int32 DebugPreviewVariant = 0;

	// --- STS|Camera ---

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "STS|Camera")
	TObjectPtr<UCameraComponent> CameraComponent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Camera")
	ESTSCameraPreset CameraPreset = ESTSCameraPreset::Oblique;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Camera", meta = (ClampMin = "-89.0", ClampMax = "-1.0"))
	float ObliquePitchDeg = -12.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Camera", meta = (ClampMin = "5.0", ClampMax = "170.0"))
	float FieldOfViewDeg = 60.0f;

	/** Places the camera so the whole grid is on screen. Ignored by the Custom preset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Camera")
	bool bAutoFrame = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Camera")
	bool bUseTestbenchCameraOnBeginPlay = true;

	// --- STS|Recorder ---

	/** Output goes to Saved/SimTexStrm/<RecordName>_<yyyyMMdd_HHmmss>.csv and _summary.txt. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Recorder")
	FString RecordName = TEXT("Run");

	/** 0 records until StopRecording. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Recorder", meta = (ClampMin = "0.0"))
	float RecordDurationSec = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Recorder")
	bool bRecordOnBeginPlay = false;

	/** Warm-up before a BeginPlay recording starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Recorder", meta = (ClampMin = "0.0"))
	float RecordStartDelaySec = 3.0f;

	/** How often process RAM and RHI texture memory are re-sampled; the last value is repeated in between. 0 samples every frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Recorder", meta = (ClampMin = "0.0"))
	float MemorySampleIntervalSec = 0.1f;

	// --- STS|Sequence ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Sequence")
	bool bRunPhase0SequenceOnBeginPlay = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Sequence", meta = (ClampMin = "0.0"))
	float SeqWarmupSec = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Sequence", meta = (ClampMin = "1.0"))
	float SeqRecordSec = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Sequence", meta = (ClampMin = "0.0"))
	float SeqSettleSec = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Sequence")
	bool bSeqIncludeDestroy = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Sequence")
	bool bSeqQuitWhenDone = false;

	// --- STS|HUD ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|HUD")
	bool bShowHud = true;

	// --- STS|Updates|Server ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Server", meta = (ClampMin = "1", ClampMax = "32"))
	int32 ServerWorkerCount = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Server", meta = (ClampMin = "0.0"))
	float ServerLatencyMs = 20.0f;

	/** Uniform +/- jitter, seeded per request ID. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Server", meta = (ClampMin = "0.0"))
	float ServerLatencyJitterMs = 5.0f;

	/** Link bandwidth in MB (1048576 bytes) per second. 0 is unlimited; 1100 is about 10GbE. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Server", meta = (ClampMin = "0.0"))
	float ServerLinkBandwidthMBps = 0.0f;

	/** Requests admitted at once; the rest wait in a backlog. 0 is unlimited. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Server", meta = (ClampMin = "0"))
	int32 ServerMaxInFlight = 0;

	/** Content variants per resolution in the payload library. Variant 0 is the plain look. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Server", meta = (ClampMin = "1", ClampMax = "16"))
	int32 VariantsPerResolution = 4;

	/** Recomputes each response's checksum on the game thread. Turn off for benchmark runs; the cost is reported separately. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Server")
	bool bVerifyChecksums = true;

	/** Debug: workers corrupt every Nth response (by request ID) so the checksum failure path can be seen. 0 is off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Server", meta = (ClampMin = "0"))
	int32 DebugCorruptEveryN = 0;

	/** Builds the payload library at BeginPlay. Otherwise it is built on first use (and always before a scenario starts). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Server")
	bool bPregeneratePayloadsOnBeginPlay = false;

	// --- STS|Updates ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates")
	int32 UpdateSeed = 12345;

	/** Reseeds before every trigger, so each trigger repeats exactly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates")
	bool bResetSeedPerTrigger = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates")
	ESTSUpdatePattern UpdatePattern = ESTSUpdatePattern::Mixed;

	/** Mixed pattern weights. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates", meta = (ClampMin = "0.0"))
	float MixedWeightSame = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates", meta = (ClampMin = "0.0"))
	float MixedWeightUp = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates", meta = (ClampMin = "0.0"))
	float MixedWeightDown = 1.0f;

	/** SameResolution wraps past the last variant to 1 instead of 0 (needs 3 or more variants). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates")
	bool bSkipVariantZero = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates")
	ESTSImpossibleStepPolicy ImpossibleStepPolicy = ESTSImpossibleStepPolicy::PickAnotherTile;

	/** Allows a tile with a request in flight to be targeted again; its older response is then superseded. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates")
	bool bAllowRetargetInFlight = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates")
	ESTSTargetMode TargetMode = ESTSTargetMode::Random;

	/** RankRange: inclusive distance rank from the grid centre (rank 0 is the nearest tile). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates", meta = (ClampMin = "0"))
	int32 RankMin = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates", meta = (ClampMin = "0"))
	int32 RankMax = 999;

	/** Patch: a rectangle of cells (both triangles of each cell). It wraps around the grid edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates")
	int32 PatchCenterX = 12;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates")
	int32 PatchCenterY = 10;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates", meta = (ClampMin = "1"))
	int32 PatchSizeX = 6;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates", meta = (ClampMin = "1"))
	int32 PatchSizeY = 6;

	/** Cells the patch moves per trigger, to simulate travel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates")
	int32 PatchStepX = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates")
	int32 PatchStepY = 0;

	// --- STS|Updates|Burst ---

	/** Counts by requested (target) resolution. With Oscillate they address the pair by its larger resolution. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Burst", meta = (ClampMin = "0"))
	int32 Burst4k = 20;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Burst", meta = (ClampMin = "0"))
	int32 Burst2k = 40;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Burst", meta = (ClampMin = "0"))
	int32 Burst1k = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Burst", meta = (ClampMin = "0"))
	int32 Burst512 = 0;

	// --- STS|Updates|Spread ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Spread", meta = (ClampMin = "0.0"))
	float SpreadRatePerSec = 5.0f;

	/** 0 spreads until StopSpread. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Spread", meta = (ClampMin = "0.0"))
	float SpreadDurationSec = 0.0f;

	/** The target resolution mix (SpreadResolutionWeights). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Spread", meta = (ClampMin = "0.0"))
	float SpreadWeight512 = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Spread", meta = (ClampMin = "0.0"))
	float SpreadWeight1k = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Spread", meta = (ClampMin = "0.0"))
	float SpreadWeight2k = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Updates|Spread", meta = (ClampMin = "0.0"))
	float SpreadWeight4k = 1.0f;

	// --- STS|Scenario ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Scenario")
	bool bRunScenarioOnBeginPlay = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Scenario")
	ESTSScenarioPreset ScenarioPreset = ESTSScenarioPreset::SteadySpread;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS|Scenario")
	TArray<FSTSScenarioStep> ScenarioSteps;

private:
	struct FHudStats
	{
		double WindowStartSec = 0.0;
		int32 Frames = 0;
		double SumFrameMs = 0.0;
		double SumGameMs = 0.0;
		double SumRenderMs = 0.0;
		double SumRHIMs = 0.0;
		double SumGpuMs = 0.0;
		float MaxFrameMs = 0.0f;
		float MaxGameMs = 0.0f;
		float MaxRenderMs = 0.0f;
		float MaxRHIMs = 0.0f;
		float MaxGpuMs = 0.0f;

		float Fps = 0.0f;
		float AvgFrameMs = 0.0f;
		float AvgGameMs = 0.0f;
		float AvgRenderMs = 0.0f;
		float AvgRHIMs = 0.0f;
		float AvgGpuMs = 0.0f;
		float ShownMaxFrameMs = 0.0f;
		float ShownMaxGameMs = 0.0f;
		float ShownMaxRenderMs = 0.0f;
		float ShownMaxRHIMs = 0.0f;
		float ShownMaxGpuMs = 0.0f;

		// Update stream, accumulated over the same window.
		int32 Issued = 0;
		int32 Released = 0;
		double ReleasedMB = 0.0;
		double LatencySumMs = 0.0;
		float LatencyMaxMs = 0.0f;
		double ConsumerSumMs = 0.0;
		float ConsumerMaxMs = 0.0f;

		float IssuedPerSec = 0.0f;
		float ReleasedPerSec = 0.0f;
		float MBPerSec = 0.0f;
		float LatencyAvgMs = 0.0f;
		float ShownLatencyMaxMs = 0.0f;
		float ConsumerAvgMs = 0.0f;
		float ShownConsumerMaxMs = 0.0f;
	};

	/** What the update stream did during one frame; becomes one CSV row's Phase 1 columns. */
	struct FUpdateFrameCounters
	{
		int32 Issued = 0;
		int32 Released = 0;
		int32 Stale = 0;
		int32 ChecksumFailures = 0;
		double ReleasedBytes = 0.0;
		double LatencySumMs = 0.0;
		double LatencyMaxMs = 0.0;
		double ConsumerMs = 0.0;
		double ChecksumMs = 0.0;
		double IssueMs = 0.0;
	};

	/** The outcome of one trigger (a burst, or the requests due in one spread frame). */
	struct FBatchResult
	{
		int32 Requested = 0;
		int32 Issued = 0;
		int32 Shortfall = 0;
		int32 Skipped = 0;
		int32 RequestedByBucket[STS::NumBuckets] = {};
		int32 IssuedByBucket[STS::NumBuckets] = {};
		uint64 SelectionHash = 0;
		double TimeSec = 0.0;
	};

	/** Totals over the active recording, for the summary. Reset when a recording starts. */
	struct FUpdateWindow
	{
		int64 Issued = 0;
		int64 Released = 0;
		int64 Stale = 0;
		int64 Late = 0;
		int64 Superseded = 0;
		int64 ChecksumFailures = 0;
		int64 ChecksumBytes = 0;
		double ReleasedBytes = 0.0;
		double ChecksumMsTotal = 0.0;
		double ChecksumMsWorst = 0.0;
		int32 BurstCount = 0;
		int64 BurstRequested = 0;
		int64 BurstIssued = 0;
		int64 SpreadRequested = 0;
		int64 SpreadIssued = 0;
		uint64 SelectionHashChain = 0;
		TArray<float> LatencyMs;
		TArray<float> ReleaseLagMs;
		TArray<float> ModelLatencyMs;
		TArray<float> ConsumerMs;
		TArray<float> ChecksumMs;
		TArray<float> IssueMs;
		TArray<FString> BurstLines;
	};

	/** A timeline-driven trigger: item k is due at StartSec + k / Rate. */
	struct FTimeline
	{
		bool bActive = false;
		double StartSec = 0.0;
		double Rate = 0.0;
		double DurationSec = 0.0;
		int64 NextIndex = 0;
		int32 BurstCounts[STS::NumBuckets] = {};
	};

	struct FSelectionContext
	{
		TArray<uint8> Selected;
		TArray<int32> SelectedList;
		int32 PatchMinX = 0;
		int32 PatchMinY = 0;
	};

	enum class EPickResult : uint8
	{
		Picked,
		Shortfall,
		Skipped
	};

	bool IsGameWorldValid(const TCHAR* Caller) const;

	// Tiles
	void ResolveGrid(int32& OutCellsX, int32& OutCellsY, bool bWarn) const;
	bool BuildTilePlan();
	void CreateTileSlice(int32 MaxTiles);
	void CreateTile(const FSTSTilePlan& Plan);
	void FinishCreation();
	UTexture2D* CreateTileTexture(int32 Resolution);
	void EnsureMeshes();
	void ResolveBaseMaterial();
	void EnsurePayloadLibrary(const TSet<int32>& Resolutions);
	FSTSTextureLook GetLookForResolution(int32 Resolution) const;

	// Camera
	void ApplyCameraPreset();
	bool TryClaimViewTarget();

	// Recording and sequence
	void OnEndFrame();
	void AccumulateHudStats(double NowSec, const FSTSFrameSample& Sample);
	void StopRecordingInternal();
	void UpdateSequence(double NowSec);
	void BeginSequenceStep(ESTSSequenceStep Step, double NowSec);
	void FinishSequence();
	void GatherSummaryHeader(TArray<FString>& OutLines) const;
	bool GatherRenderSettings(TArray<FString>& OutLines, FString& OutProfileSummary) const;
	void LogRenderSettings() const;

	// HUD
	void DrawHud(UCanvas* Canvas, APlayerController* Controller);

	// Server and payload library (STSTestbenchUpdates.cpp)
	bool EnsureServer();
	bool EnsureServerLibrary();
	void ShutdownServer();
	void ApplyServerSettings();
	bool IsServerIdle() const;

	// Triggers and target selection (STSTestbenchUpdates.cpp)
	void UpdateTimelines(double NowSec);
	/** Counts has STS::NumBuckets entries, indexed by target resolution bucket. */
	void IssueBurst(const int32* Counts, double NowSec, const TCHAR* Source);

	/** Targets pair a requested resolution with the time the request is issued (its timeline due time for a spread). */
	void IssueBatch(const TArray<TPair<int32, double>>& Targets, bool bBurst, const TCHAR* Source);
	EPickResult PickTile(int32 TargetResolution, FSelectionContext& Context, int32& OutTile, int32& OutResolution, int32& OutVariant);
	void CollectPool(ESTSUpdatePattern Pattern, int32 TargetResolution, const FSelectionContext& Context, TArray<int32>& OutPool) const;
	bool IsTileSelectable(int32 TileIndex, const FSelectionContext& Context) const;
	int32 NextSameResolutionVariant(int32 Variant) const;
	int32 PickSpreadResolution();
	void ResetTriggerState();
	void ResetLogicalState();

	// Consumer (STSTestbenchUpdates.cpp)
	void ConsumeResponses(double NowSec);
	void ProcessResponse(const FSTSResponse& Response, double NowSec);
	void FillUpdateColumns(FSTSFrameSample& Sample);
	void GatherUpdateSummary(TArray<FString>& OutLines) const;
	void AppendUpdateHud(TArray<TPair<FString, FLinearColor>>& Lines, double NowSec) const;

	// Scenario (STSTestbenchScenario.cpp)
	void BuildScenarioPreset(ESTSScenarioPreset Preset, TArray<FSTSScenarioStep>& OutSteps) const;
	void UpdateScenario(double NowSec);
	bool ExecuteScenarioStep(const FSTSScenarioStep& Step, bool bFirstFrame, double NowSec);
	void FinishScenario(const TCHAR* Reason);
	void RequestQuit(const TCHAR* Reason);

	UPROPERTY(VisibleAnywhere, Category = "STS")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> LowerLeftMesh;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> UpperRightMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ResolvedBaseMaterial;

	UPROPERTY(Transient)
	TArray<FSTSTile> Tiles;

	float BuiltMeshCellSizeCm = 0.0f;
	FString MaterialWarning;

	TArray<FSTSTilePlan> TilePlans;
	int32 NextTileToCreate = 0;
	bool bCreationInProgress = false;
	double CreationPrepSec = 0.0;
	double CreationBusySec = 0.0;
	double CreationWorstSliceSec = 0.0;
	int32 CreationSliceCount = 0;
	double LastCreationMs = 0.0;

	TMap<int32, TSharedPtr<const FSTSMipChain>> PayloadLibrary;
	TMap<int32, FSTSTextureLook> PayloadLooks;

	int32 TileCountsPerBucket[STS::NumBuckets] = {};
	int32 TileCountsOtherResolution = 0;

	FSTSRecorder Recorder;
	FDelegateHandle EndFrameHandle;
	FDelegateHandle HudHandle;
	double LastEndFrameSec = 0.0;
	double LastMemorySampleSec = -1.0e9;
	float CachedProcessPhysMB = 0.0f;
	float CachedRHITextureMB = 0.0f;
	FHudStats HudStats;

	bool bAutoRecordPending = false;
	double AutoRecordStartSec = 0.0;
	bool bViewTargetPending = false;
	bool bBenchmarkProfileApplied = false;
	FString RenderProfileSummary;
	double LastProfileRefreshSec = -1.0e9;

	ESTSSequenceStep SequenceStep = ESTSSequenceStep::Idle;
	double SequenceStepStartSec = 0.0;
	bool bSequenceQuitWhenDone = false;

	// --- Phase 1: server, update stream, scenario ---

	TUniquePtr<FSTSTerrainServer> Server;
	TSharedPtr<const FSTSPayloadLibrary> ServerLibrary;
	TArray<FSTSTextureLook> ServerLibraryLooks;
	int32 ServerLibraryVariants = 0;
	int32 PayloadLibraryVariant = 0;

	/** Bumped whenever tiles are destroyed; responses of an older epoch are discarded. */
	uint32 TileEpoch = 0;
	int64 NextRequestId = 0;
	int32 ActiveCellsX = 1;
	int32 ActiveCellsY = 1;
	int32 LogicalCountsPerBucket[STS::NumBuckets] = {};

	FRandomStream UpdateStream;
	int32 PatchTriggerCount = 0;
	FTimeline Spread;
	FTimeline BurstRepeat;
	TArray<int32> PoolScratch[3];
	TArray<FSTSResponse> ReleasedScratch;

	FUpdateFrameCounters Frame;
	FUpdateWindow Window;
	FBatchResult LastBurst;
	bool bHasLastBurst = false;
	FSTSServerCounters LastServerCounters;
	int64 TotalChecksumFailures = 0;
	int64 TotalRequestsIssued = 0;
	int64 TotalStaleDiscarded = 0;
	int32 LoggedChecksumFailures = 0;
	double TotalChecksumMs = 0.0;
	double TotalChecksumBytes = 0.0;
	double LastChecksumMsPerMB = 0.0;

	TArray<FSTSScenarioStep> ScenarioRuntimeSteps;
	bool bScenarioRunning = false;
	bool bScenarioStepEntered = false;
	bool bScenarioQuitWhenDone = false;
	int32 ScenarioStepIndex = 0;
	double ScenarioStepStartSec = 0.0;
};

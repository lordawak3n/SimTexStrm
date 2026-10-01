// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "STSDxt1Generator.h"
#include "STSRecorder.h"
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

	int32 Resolution = 0;
};

/** Where a tile will be created; resolved up front so time-sliced creation is deterministic. */
struct FSTSTilePlan
{
	int32 CellX = 0;
	int32 CellY = 0;
	bool bUpper = false;
	int32 Resolution = 512;
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

	void StartRecordingNamed(const FString& Name, double DurationSec);
	void SetHudVisible(bool bVisible) { bShowHud = bVisible; }
	bool IsHudVisible() const { return bShowHud; }

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
};

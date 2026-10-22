// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSTestbench.h"

#include "SimTextureStrm.h"
#include "Camera/CameraComponent.h"
#include "CanvasItem.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Debug/DebugDrawService.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "RHI.h"
#include "RHIStats.h"
#include "RenderTimer.h"
#include "RenderUtils.h"
#include "StaticMeshAttributes.h"
#include "TextureResource.h"
#include "UObject/UObjectGlobals.h"
#include "UnrealClient.h"

namespace
{
	constexpr int32 MaxTileCount = 10000;
	constexpr double HudWindowSec = 0.5;

	float CyclesToMs(uint32 Cycles)
	{
		return float(FPlatformTime::ToMilliseconds(Cycles));
	}

	double GetBucketChainMB(int32 Bucket)
	{
		static double ChainMB[STS::NumBuckets] = { -1.0 };
		if (ChainMB[0] < 0.0)
		{
			for (int32 Index = 0; Index < STS::NumBuckets; ++Index)
			{
				const int32 Resolution = STS::BucketResolutions[Index];
				ChainMB[Index] = double(FSTSDxt1Generator::GetChainBytes(Resolution, Resolution)) / STS::MegaByte;
			}
		}
		return ChainMB[Bucket];
	}

	const TCHAR* GetSequenceStepName(ESTSSequenceStep Step)
	{
		switch (Step)
		{
		case ESTSSequenceStep::Warmup: return TEXT("Warmup");
		case ESTSSequenceStep::RecordEmpty: return TEXT("Record Empty");
		case ESTSSequenceStep::WaitForCreate: return TEXT("Creating tiles");
		case ESTSSequenceStep::SettleTiles: return TEXT("Settle (tiles)");
		case ESTSSequenceStep::RecordTiles: return TEXT("Record Tiles");
		case ESTSSequenceStep::SettleDestroy: return TEXT("Settle (destroyed)");
		case ESTSSequenceStep::RecordAfterDestroy: return TEXT("Record AfterDestroy");
		default: return TEXT("Idle");
		}
	}

	UStaticMesh* BuildTriangleMesh(UObject* Outer, bool bUpper, float CellSize)
	{
		const FName SlotName(TEXT("Tile"));

		// Both hypotenuses lie on u + v = 1. UE front faces wind counter-clockwise seen from the front
		// (FMeshDescription::ComputePolygonPlane gives +Z for this order, and D3D12RHI sets FrontCounterClockwise).
		const FVector2f Positions[2][3] =
		{
			{ FVector2f(0.0f, 0.0f), FVector2f(0.0f, CellSize), FVector2f(CellSize, 0.0f) },
			{ FVector2f(CellSize, 0.0f), FVector2f(0.0f, CellSize), FVector2f(CellSize, CellSize) }
		};
		const FVector2f UVs[2][3] =
		{
			{ FVector2f(0.0f, 0.0f), FVector2f(0.0f, 1.0f), FVector2f(1.0f, 0.0f) },
			{ FVector2f(1.0f, 0.0f), FVector2f(0.0f, 1.0f), FVector2f(1.0f, 1.0f) }
		};
		const int32 Shape = bUpper ? 1 : 0;

		FMeshDescription MeshDescription;
		FStaticMeshAttributes Attributes(MeshDescription);
		Attributes.Register();

		TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector2f> InstanceUVs = Attributes.GetVertexInstanceUVs();
		TVertexInstanceAttributesRef<FVector3f> InstanceNormals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> InstanceTangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> InstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> InstanceColors = Attributes.GetVertexInstanceColors();
		InstanceUVs.SetNumChannels(1);

		const FPolygonGroupID GroupID = MeshDescription.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[GroupID] = SlotName;

		FVertexInstanceID Instances[3];
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const FVertexID VertexID = MeshDescription.CreateVertex();
			VertexPositions[VertexID] = FVector3f(Positions[Shape][Corner].X, Positions[Shape][Corner].Y, 0.0f);

			Instances[Corner] = MeshDescription.CreateVertexInstance(VertexID);
			InstanceUVs.Set(Instances[Corner], 0, UVs[Shape][Corner]);
			InstanceNormals[Instances[Corner]] = FVector3f(0.0f, 0.0f, 1.0f);
			InstanceTangents[Instances[Corner]] = FVector3f(1.0f, 0.0f, 0.0f);
			InstanceBinormalSigns[Instances[Corner]] = 1.0f;
			InstanceColors[Instances[Corner]] = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		}
		MeshDescription.CreateTriangle(GroupID, Instances);

		UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, NAME_None, RF_Transient);
		Mesh->GetStaticMaterials().Add(FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface), SlotName));

		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bUseHashAsGuid = false;
		Params.bBuildSimpleCollision = false;
		Params.bCommitMeshDescription = false;
		Params.bFastBuild = true;

		TArray<const FMeshDescription*> Descriptions;
		Descriptions.Add(&MeshDescription);
		if (!Mesh->BuildFromMeshDescriptions(Descriptions, Params))
		{
			UE_LOG(LogSTS, Error, TEXT("Failed to build the %s triangle mesh."), bUpper ? TEXT("upper-right") : TEXT("lower-left"));
		}
		return Mesh;
	}
}

ASTSTestbench::ASTSTestbench()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SceneRoot->SetMobility(EComponentMobility::Static);
	SetRootComponent(SceneRoot);

	CameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	CameraComponent->SetupAttachment(SceneRoot);
	CameraComponent->bConstrainAspectRatio = false;
}

void ASTSTestbench::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ApplyCameraPreset();
}

void ASTSTestbench::BeginPlay()
{
	Super::BeginPlay();

	bool bRunSequence = bRunPhase0SequenceOnBeginPlay;
	bSequenceQuitWhenDone = bSeqQuitWhenDone;
	if (FParse::Param(FCommandLine::Get(), TEXT("STSSequence")))
	{
		bRunSequence = true;
		bSequenceQuitWhenDone = true;
	}

	bool bRunScenario = bRunScenarioOnBeginPlay;
	bScenarioQuitWhenDone = false;
	FString ScenarioName;
	if (FParse::Value(FCommandLine::Get(), TEXT("STSScenario="), ScenarioName) && !ScenarioName.IsEmpty())
	{
		const int64 PresetValue = StaticEnum<ESTSScenarioPreset>()->GetValueByNameString(ScenarioName);
		if (PresetValue == INDEX_NONE || PresetValue == int64(ESTSScenarioPreset::Custom))
		{
			UE_LOG(LogSTS, Error, TEXT("-STSScenario=%s is not a scenario preset; ignored."), *ScenarioName);
		}
		else
		{
			ScenarioPreset = ESTSScenarioPreset(PresetValue);
			ApplyScenarioPreset();
			bRunScenario = true;
			bScenarioQuitWhenDone = true;
			if (bRunSequence)
			{
				UE_LOG(LogSTS, Warning, TEXT("Both -STSScenario and -STSSequence were given; running the scenario only."));
				bRunSequence = false;
			}
		}
	}

	// Command-line overrides, so unattended -game runs can change server knobs without editing the Blueprint.
	int32 ChecksumOverride = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("STSChecksums="), ChecksumOverride))
	{
		bVerifyChecksums = ChecksumOverride != 0;
	}
	FParse::Value(FCommandLine::Get(), TEXT("STSWorkers="), ServerWorkerCount);
	FParse::Value(FCommandLine::Get(), TEXT("STSLatencyMs="), ServerLatencyMs);
	FParse::Value(FCommandLine::Get(), TEXT("STSJitterMs="), ServerLatencyJitterMs);
	FParse::Value(FCommandLine::Get(), TEXT("STSBandwidthMBps="), ServerLinkBandwidthMBps);
	FParse::Value(FCommandLine::Get(), TEXT("STSMaxInFlight="), ServerMaxInFlight);
	FParse::Value(FCommandLine::Get(), TEXT("STSCorruptEveryN="), DebugCorruptEveryN);

	UpdateStream.Initialize(UpdateSeed);
	ResetTriggerState();

	LogRenderSettings();
	ResolveBaseMaterial();
	ApplyCameraPreset();
	if (bUseTestbenchCameraOnBeginPlay && !TryClaimViewTarget())
	{
		bViewTargetPending = true;
	}

	LastEndFrameSec = 0.0;
	EndFrameHandle = FCoreDelegates::OnEndFrame.AddUObject(this, &ASTSTestbench::OnEndFrame);
	HudHandle = UDebugDrawService::Register(TEXT("Game"), FDebugDrawDelegate::CreateUObject(this, &ASTSTestbench::DrawHud));

	if (bCreateTilesOnBeginPlay)
	{
		CreateTiles();
	}

	if (bPregeneratePayloadsOnBeginPlay)
	{
		EnsureServer();
		EnsureServerLibrary();
	}

	if (bRunScenario)
	{
		RunScenario();
	}
	else if (bRunSequence)
	{
		RunPhase0Sequence();
	}
	else if (bRecordOnBeginPlay)
	{
		bAutoRecordPending = true;
		AutoRecordStartSec = FPlatformTime::Seconds() + RecordStartDelaySec;
	}
}

void ASTSTestbench::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	FCoreDelegates::OnEndFrame.Remove(EndFrameHandle);
	EndFrameHandle.Reset();
	UDebugDrawService::Unregister(HudHandle);
	HudHandle.Reset();

	bScenarioRunning = false;
	Spread.bActive = false;
	BurstRepeat.bActive = false;

	if (Recorder.IsRecording())
	{
		StopRecordingInternal();
	}

	// Stops the workers and frees every buffer still queued, in flight or waiting in the delay line.
	ShutdownServer();

	bCreationInProgress = false;
	bAutoRecordPending = false;
	SequenceStep = ESTSSequenceStep::Idle;
	TilePlans.Empty();
	Tiles.Empty();

	Super::EndPlay(EndPlayReason);
}

void ASTSTestbench::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const double NowSec = FPlatformTime::Seconds();

	if (bViewTargetPending && TryClaimViewTarget())
	{
		bViewTargetPending = false;
	}

	if (bCreationInProgress)
	{
		CreateTileSlice(TilesCreatedPerFrame);
	}

	ConsumeResponses(NowSec);
	UpdateScenario(NowSec);
	UpdateTimelines(NowSec);

	if (bAutoRecordPending && NowSec >= AutoRecordStartSec)
	{
		bAutoRecordPending = false;
		StartRecording();
	}

	if (Recorder.IsDurationElapsed(NowSec))
	{
		StopRecordingInternal();
	}

	UpdateSequence(NowSec);
}

bool ASTSTestbench::IsGameWorldValid(const TCHAR* Caller) const
{
	const UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		UE_LOG(LogSTS, Warning, TEXT("%s ignored: only available in a game world (PIE or -game)."), Caller);
		return false;
	}
	return true;
}

// --- Tiles ---

void ASTSTestbench::ResolveGrid(int32& OutCellsX, int32& OutCellsY, bool bWarn) const
{
	const int64 MaxCells = MaxTileCount / 2;
	int64 CellsX = FMath::Max(1, GridCellsX);
	int64 CellsY = FMath::Max(1, GridCellsY);
	CellsX = FMath::Min(CellsX, MaxCells);
	CellsY = FMath::Max<int64>(1, FMath::Min(CellsY, MaxCells / CellsX));

	OutCellsX = int32(CellsX);
	OutCellsY = int32(CellsY);
	if (bWarn && (OutCellsX != GridCellsX || OutCellsY != GridCellsY))
	{
		UE_LOG(LogSTS, Warning, TEXT("Grid %dx%d exceeds the %d tile limit; clamped to %dx%d."), GridCellsX, GridCellsY, MaxTileCount, OutCellsX, OutCellsY);
	}
}

void ASTSTestbench::ResolveBaseMaterial()
{
	MaterialWarning.Reset();

	UMaterialInterface* Base = TileMaterial;
	if (!Base)
	{
		MaterialWarning = TEXT("TileMaterial is not set; using the engine default material.");
	}
	else
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		Base->GetAllTextureParameterInfo(Infos, Ids);
		const bool bHasParameter = Infos.ContainsByPredicate([this](const FMaterialParameterInfo& Info)
		{
			return Info.Name == TextureParameterName;
		});
		if (!bHasParameter)
		{
			MaterialWarning = FString::Printf(TEXT("TileMaterial '%s' has no texture parameter '%s'; using the engine default material."), *Base->GetName(), *TextureParameterName.ToString());
			Base = nullptr;
		}
	}

	if (!MaterialWarning.IsEmpty())
	{
		UE_LOG(LogSTS, Warning, TEXT("%s"), *MaterialWarning);
	}
	ResolvedBaseMaterial = Base ? Base : UMaterial::GetDefaultMaterial(MD_Surface);
}

void ASTSTestbench::EnsureMeshes()
{
	if (LowerLeftMesh && UpperRightMesh && BuiltMeshCellSizeCm == CellSizeCm)
	{
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(STS_BuildMeshes);
	const double StartSec = FPlatformTime::Seconds();
	LowerLeftMesh = BuildTriangleMesh(this, false, CellSizeCm);
	UpperRightMesh = BuildTriangleMesh(this, true, CellSizeCm);
	BuiltMeshCellSizeCm = CellSizeCm;
	UE_LOG(LogSTS, Log, TEXT("Built the triangle meshes (cell %.0f cm) in %.2f ms."), CellSizeCm, (FPlatformTime::Seconds() - StartSec) * 1000.0);
}

FSTSTextureLook ASTSTestbench::GetLookForResolution(int32 Resolution) const
{
	FSTSTextureLook Look;
	switch (Resolution)
	{
	case 512: Look.BaseColor = Color512; break;
	case 1024: Look.BaseColor = Color1k; break;
	case 2048: Look.BaseColor = Color2k; break;
	case 4096: Look.BaseColor = Color4k; break;
	default: Look.BaseColor = FColor::Magenta; break;
	}
	Look.BorderColor = BorderColor;
	Look.BorderWidthFraction = BorderWidthFraction;
	Look.bDrawDiagonal = bDrawDiagonal;
	Look.MipCoding = MipCoding;
	Look.MipBrightnessLow = MipBrightnessLow;
	return Look;
}

void ASTSTestbench::EnsurePayloadLibrary(const TSet<int32>& Resolutions)
{
	if (PayloadLibraryVariant != DebugPreviewVariant)
	{
		PayloadLibrary.Reset();
		PayloadLooks.Reset();
		PayloadLibraryVariant = DebugPreviewVariant;
	}

	for (const int32 Resolution : Resolutions)
	{
		const FSTSTextureLook Look = GetLookForResolution(Resolution);
		const FSTSTextureLook* Existing = PayloadLooks.Find(Resolution);
		if (Existing && *Existing == Look && PayloadLibrary.Contains(Resolution))
		{
			continue;
		}

		const double StartSec = FPlatformTime::Seconds();
		TSharedRef<FSTSMipChain> Chain = MakeShared<FSTSMipChain>();
		FSTSDxt1Generator::GenerateChain(Resolution, Resolution, Look, uint32(FMath::Max(0, PayloadLibraryVariant)), *Chain);
		UE_LOG(LogSTS, Log, TEXT("Generated the %d payload (variant %d): %d mips, %.2f MB in %.1f ms."), Resolution, PayloadLibraryVariant, Chain->Mips.Num(), Chain->TotalBytes / STS::MegaByte, (FPlatformTime::Seconds() - StartSec) * 1000.0);

		PayloadLibrary.Add(Resolution, Chain);
		PayloadLooks.Add(Resolution, Look);
	}
}

bool ASTSTestbench::BuildTilePlan()
{
	int32 CellsX = 0;
	int32 CellsY = 0;
	ResolveGrid(CellsX, CellsY, true);

	const int32 NumTiles = 2 * CellsX * CellsY;
	const double CellSize = CellSizeCm;
	const double HalfX = CellsX * CellSize * 0.5;
	const double HalfY = CellsY * CellSize * 0.5;

	TilePlans.Reset(NumTiles);
	TArray<double> Distances;
	Distances.Reserve(NumTiles);
	for (int32 CellY = 0; CellY < CellsY; ++CellY)
	{
		for (int32 CellX = 0; CellX < CellsX; ++CellX)
		{
			for (int32 Half = 0; Half < 2; ++Half)
			{
				FSTSTilePlan& Plan = TilePlans.AddDefaulted_GetRef();
				Plan.CellX = CellX;
				Plan.CellY = CellY;
				Plan.bUpper = Half == 1;
				Plan.CellOrigin = FVector(CellX * CellSize - HalfX, CellY * CellSize - HalfY, 0.0);

				const double CentroidOffset = Plan.bUpper ? CellSize * 2.0 / 3.0 : CellSize / 3.0;
				Distances.Add(FVector2D(Plan.CellOrigin.X + CentroidOffset, Plan.CellOrigin.Y + CentroidOffset).Size());
			}
		}
	}

	TArray<int32> Order;
	Order.SetNumUninitialized(NumTiles);
	for (int32 Index = 0; Index < NumTiles; ++Index)
	{
		Order[Index] = Index;
	}
	Order.Sort([&Distances](int32 A, int32 B)
	{
		return Distances[A] < Distances[B] || (Distances[A] == Distances[B] && A < B);
	});
	for (int32 Rank = 0; Rank < NumTiles; ++Rank)
	{
		TilePlans[Order[Rank]].Rank = Rank;
	}
	ActiveCellsX = CellsX;
	ActiveCellsY = CellsY;

	int32 Remainder = RemainderResolution;
	if (STS::ResolutionToBucket(Remainder) == INDEX_NONE)
	{
		UE_LOG(LogSTS, Warning, TEXT("RemainderResolution %d is not 512, 1024, 2048 or 4096; using 512."), Remainder);
		Remainder = 512;
	}

	const int32 Counts[STS::NumBuckets] = { Count512, Count1k, Count2k, Count4k };
	int64 Requested = 0;
	int32 Rank = 0;
	for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
	{
		const int32 Count = FMath::Max(0, Counts[Bucket]);
		Requested += Count;
		for (int32 Index = 0; Index < Count && Rank < NumTiles; ++Index)
		{
			TilePlans[Order[Rank++]].Resolution = STS::BucketResolutions[Bucket];
		}
	}
	if (Requested > NumTiles)
	{
		UE_LOG(LogSTS, Warning, TEXT("Resolution counts total %lld but only %d tiles exist; filling from 4k down."), Requested, NumTiles);
	}
	for (; Rank < NumTiles; ++Rank)
	{
		TilePlans[Order[Rank]].Resolution = Remainder;
	}

	TSet<int32> Needed;
	for (const FSTSTilePlan& Plan : TilePlans)
	{
		Needed.Add(Plan.Resolution);
	}
	EnsurePayloadLibrary(Needed);
	return true;
}

void ASTSTestbench::CreateTiles()
{
	if (!IsGameWorldValid(TEXT("CreateTiles")))
	{
		return;
	}
	if (bCreationInProgress || Tiles.Num() > 0)
	{
		UE_LOG(LogSTS, Warning, TEXT("CreateTiles ignored: %d tiles already exist or are being created. Use RebuildTiles."), Tiles.Num());
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(STS_CreateTiles);
	const double StartSec = FPlatformTime::Seconds();

	ResolveBaseMaterial();
	EnsureMeshes();
	BuildTilePlan();

	CreationPrepSec = FPlatformTime::Seconds() - StartSec;
	CreationBusySec = 0.0;
	CreationWorstSliceSec = 0.0;
	CreationSliceCount = 0;
	NextTileToCreate = 0;
	bCreationInProgress = true;
	CreateTileSlice(TilesCreatedPerFrame);
}

void ASTSTestbench::CreateTileSlice(int32 MaxTiles)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(STS_CreateTileSlice);
	const double StartSec = FPlatformTime::Seconds();

	const int32 End = MaxTiles > 0 ? FMath::Min(TilePlans.Num(), NextTileToCreate + MaxTiles) : TilePlans.Num();
	Tiles.Reserve(TilePlans.Num());
	for (; NextTileToCreate < End; ++NextTileToCreate)
	{
		CreateTile(TilePlans[NextTileToCreate]);
	}

	const double ElapsedSec = FPlatformTime::Seconds() - StartSec;
	CreationBusySec += ElapsedSec;
	CreationWorstSliceSec = FMath::Max(CreationWorstSliceSec, ElapsedSec);
	++CreationSliceCount;

	if (NextTileToCreate >= TilePlans.Num())
	{
		FinishCreation();
	}
}

void ASTSTestbench::CreateTile(const FSTSTilePlan& Plan)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(STS_CreateTile);

	UTexture2D* Texture = CreateTileTexture(Plan.Resolution);
	if (!Texture)
	{
		UE_LOG(LogSTS, Error, TEXT("Skipping tile (%d,%d): no texture could be created."), Plan.CellX, Plan.CellY);
		return;
	}

	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
	Component->SetMobility(TileMobility);
	Component->SetStaticMesh(Plan.bUpper ? UpperRightMesh : LowerLeftMesh);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetGenerateOverlapEvents(false);
	Component->SetCastShadow(bTilesCastShadows);
	Component->SetupAttachment(SceneRoot);
	Component->SetRelativeLocation(Plan.CellOrigin);

	UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(ResolvedBaseMaterial, this);
	Material->SetTextureParameterValue(TextureParameterName, Texture);
	Component->SetMaterial(0, Material);
	Component->RegisterComponent();

	FSTSTile& Tile = Tiles.AddDefaulted_GetRef();
	Tile.Component = Component;
	Tile.Material = Material;
	Tile.Texture = Texture;
	Tile.Resolution = Plan.Resolution;
	Tile.CellX = Plan.CellX;
	Tile.CellY = Plan.CellY;
	Tile.Rank = Plan.Rank;
	Tile.LogicalResolution = Plan.Resolution;

	const int32 Bucket = STS::ResolutionToBucket(Plan.Resolution);
	if (Bucket != INDEX_NONE)
	{
		++TileCountsPerBucket[Bucket];
		++LogicalCountsPerBucket[Bucket];
	}
	else
	{
		++TileCountsOtherResolution;
	}
}

UTexture2D* ASTSTestbench::CreateTileTexture(int32 Resolution)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(STS_CreateTileTexture);

	const TSharedPtr<const FSTSMipChain>* Found = PayloadLibrary.Find(Resolution);
	if (!Found || !Found->IsValid())
	{
		return nullptr;
	}
	const FSTSMipChain& Chain = **Found;

	UTexture2D* Texture = UTexture2D::CreateTransient(Resolution, Resolution, PF_DXT1);
	if (!Texture)
	{
		return nullptr;
	}

	FTexturePlatformData* PlatformData = Texture->GetPlatformData();
	{
		FTexture2DMipMap& BaseMip = PlatformData->Mips[0];
		void* Dest = BaseMip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Dest, Chain.Mips[0].GetData(), Chain.Mips[0].Num());
		BaseMip.BulkData.Unlock();
	}
	for (int32 MipIndex = 1; MipIndex < Chain.Mips.Num(); ++MipIndex)
	{
		const int32 MipSize = FMath::Max(1, Resolution >> MipIndex);
		FTexture2DMipMap* Mip = new FTexture2DMipMap(MipSize, MipSize, 1);
		PlatformData->Mips.Add(Mip);
		Mip->BulkData.Lock(LOCK_READ_WRITE);
		void* Dest = Mip->BulkData.Realloc(Chain.Mips[MipIndex].Num());
		FMemory::Memcpy(Dest, Chain.Mips[MipIndex].GetData(), Chain.Mips[MipIndex].Num());
		Mip->BulkData.Unlock();
	}

	Texture->SRGB = bSRGB;
	Texture->LODGroup = TEXTUREGROUP_World;
	Texture->NeverStream = true;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(STS_UpdateResource);
		Texture->UpdateResource();
	}
	return Texture;
}

void ASTSTestbench::FinishCreation()
{
	bCreationInProgress = false;
	TilePlans.Empty();

	LastCreationMs = (CreationPrepSec + CreationBusySec) * 1000.0;
	UE_LOG(LogSTS, Log, TEXT("Created %d tiles in %.1f ms on the game thread (prep %.1f ms, tiles %.1f ms over %d frame(s), worst slice %.1f ms)."),
		Tiles.Num(), LastCreationMs, CreationPrepSec * 1000.0, CreationBusySec * 1000.0, CreationSliceCount, CreationWorstSliceSec * 1000.0);
	TRACE_BOOKMARK(TEXT("STS Tiles Created: %d"), Tiles.Num());
}

void ASTSTestbench::DestroyTiles()
{
	if (!IsGameWorldValid(TEXT("DestroyTiles")))
	{
		return;
	}
	if (Tiles.IsEmpty() && !bCreationInProgress)
	{
		UE_LOG(LogSTS, Log, TEXT("DestroyTiles: no tiles exist."));
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(STS_DestroyTiles);
	const double StartSec = FPlatformTime::Seconds();

	const int32 NumDestroyed = Tiles.Num();
	bCreationInProgress = false;
	TilePlans.Empty();
	for (FSTSTile& Tile : Tiles)
	{
		if (Tile.Component)
		{
			Tile.Component->DestroyComponent();
		}
	}
	Tiles.Empty();
	FMemory::Memzero(TileCountsPerBucket);
	TileCountsOtherResolution = 0;
	ResetLogicalState();
	TRACE_BOOKMARK(TEXT("STS Tiles Destroyed: %d"), NumDestroyed);

	const double DestroyedSec = FPlatformTime::Seconds();
	if (bForceGarbageCollectionOnDestroy)
	{
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	}
	UE_LOG(LogSTS, Log, TEXT("Destroyed %d tiles in %.1f ms; garbage collection %.1f ms."),
		NumDestroyed, (DestroyedSec - StartSec) * 1000.0, (FPlatformTime::Seconds() - DestroyedSec) * 1000.0);
}

void ASTSTestbench::RebuildTiles()
{
	if (!IsGameWorldValid(TEXT("RebuildTiles")))
	{
		return;
	}
	DestroyTiles();
	CreateTiles();
}

void ASTSTestbench::RegenerateTextures()
{
	if (!IsGameWorldValid(TEXT("RegenerateTextures")))
	{
		return;
	}
	if (bCreationInProgress)
	{
		UE_LOG(LogSTS, Warning, TEXT("RegenerateTextures ignored: tile creation is in progress."));
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(STS_RegenerateTextures);
	const double StartSec = FPlatformTime::Seconds();

	PayloadLibrary.Reset();
	PayloadLooks.Reset();
	ServerLibrary.Reset();

	TSet<int32> Needed;
	if (Tiles.IsEmpty())
	{
		for (const int32 Resolution : STS::BucketResolutions)
		{
			Needed.Add(Resolution);
		}
	}
	for (const FSTSTile& Tile : Tiles)
	{
		Needed.Add(Tile.Resolution);
	}
	EnsurePayloadLibrary(Needed);

	for (FSTSTile& Tile : Tiles)
	{
		UTexture2D* Texture = CreateTileTexture(Tile.Resolution);
		if (Texture)
		{
			Tile.Texture = Texture;
			Tile.Material->SetTextureParameterValue(TextureParameterName, Texture);
		}
	}

	if (!Tiles.IsEmpty() && bForceGarbageCollectionOnDestroy)
	{
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	}
	UE_LOG(LogSTS, Log, TEXT("Regenerated payloads and %d textures in %.1f ms."), Tiles.Num(), (FPlatformTime::Seconds() - StartSec) * 1000.0);
}

// --- Camera ---

void ASTSTestbench::ApplyCameraPreset()
{
	if (!CameraComponent)
	{
		return;
	}

	CameraComponent->SetFieldOfView(FieldOfViewDeg);
	if (CameraPreset == ESTSCameraPreset::Custom)
	{
		return;
	}

	const double PitchDeg = CameraPreset == ESTSCameraPreset::TopDown ? -90.0 : double(ObliquePitchDeg);
	CameraComponent->SetRelativeRotation(FRotator(PitchDeg, 0.0, 0.0));
	if (!bAutoFrame)
	{
		return;
	}

	double Aspect = 16.0 / 9.0;
	if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		const FIntPoint Size = GEngine->GameViewport->Viewport->GetSizeXY();
		if (Size.X > 0 && Size.Y > 0)
		{
			Aspect = double(Size.X) / double(Size.Y);
		}
	}

	int32 CellsX = 0;
	int32 CellsY = 0;
	ResolveGrid(CellsX, CellsY, false);
	const double HalfLength = CellsX * double(CellSizeCm) * 0.5;
	const double HalfWidth = CellsY * double(CellSizeCm) * 0.5;

	// Screen right is +Y and the camera sits on -X looking along +X, pitched down; the distance grows until all four grid corners fit.
	const double TanHalfH = FMath::Tan(FMath::DegreesToRadians(double(FieldOfViewDeg)) * 0.5);
	const double TanHalfV = TanHalfH / Aspect;
	const double Pitch = FMath::DegreesToRadians(PitchDeg);
	const double ForwardX = FMath::Cos(Pitch);
	const double ForwardZ = FMath::Sin(Pitch);
	const double UpX = -ForwardZ;
	const double UpZ = ForwardX;
	constexpr double Margin = 0.95;

	double Distance = CellSizeCm;
	for (int32 Iteration = 0; Iteration < 4000; ++Iteration)
	{
		const double CameraX = -ForwardX * Distance;
		const double CameraZ = -ForwardZ * Distance;
		bool bFits = true;
		for (int32 Corner = 0; Corner < 4 && bFits; ++Corner)
		{
			const double DeltaX = ((Corner & 1) ? HalfLength : -HalfLength) - CameraX;
			const double DeltaY = (Corner & 2) ? HalfWidth : -HalfWidth;
			const double DeltaZ = -CameraZ;
			const double Depth = DeltaX * ForwardX + DeltaZ * ForwardZ;
			const double Up = DeltaX * UpX + DeltaZ * UpZ;
			bFits = Depth > 1.0 && FMath::Abs(DeltaY) <= Depth * TanHalfH * Margin && FMath::Abs(Up) <= Depth * TanHalfV * Margin;
		}
		if (bFits)
		{
			break;
		}
		Distance *= 1.01;
	}

	CameraComponent->SetRelativeLocation(FVector(-ForwardX * Distance, 0.0, -ForwardZ * Distance));
}

bool ASTSTestbench::TryClaimViewTarget()
{
	APlayerController* Controller = UGameplayStatics::GetPlayerController(this, 0);
	if (!Controller)
	{
		return false;
	}
	Controller->SetViewTarget(this);
	return true;
}

// --- Recording and sequence ---

void ASTSTestbench::StartRecording()
{
	StartRecordingNamed(RecordName, RecordDurationSec);
}

void ASTSTestbench::StartRecordingNamed(const FString& Name, double DurationSec)
{
	if (!IsGameWorldValid(TEXT("StartRecording")))
	{
		return;
	}
	if (Recorder.IsRecording())
	{
		UE_LOG(LogSTS, Warning, TEXT("StartRecording ignored: already recording '%s'."), *Recorder.GetName());
		return;
	}

	const FString OutputDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SimTexStrm"));
	if (Recorder.Start(Name, DurationSec, FPlatformTime::Seconds(), OutputDir))
	{
		Window = FUpdateWindow();
	}
}

void ASTSTestbench::StopRecording()
{
	if (!Recorder.IsRecording())
	{
		UE_LOG(LogSTS, Log, TEXT("StopRecording: not recording."));
		return;
	}
	StopRecordingInternal();
}

void ASTSTestbench::StopRecordingInternal()
{
	TArray<FString> HeaderLines;
	GatherSummaryHeader(HeaderLines);
	Recorder.Stop(HeaderLines);
}

void ASTSTestbench::OnEndFrame()
{
	const double NowSec = FPlatformTime::Seconds();
	if (LastEndFrameSec <= 0.0)
	{
		LastEndFrameSec = NowSec;
		HudStats.WindowStartSec = NowSec;
		return;
	}

	FSTSFrameSample Sample;
	Sample.FrameMs = float((NowSec - LastEndFrameSec) * 1000.0);
	LastEndFrameSec = NowSec;

	Sample.GameThreadMs = CyclesToMs(GGameThreadTime);
	Sample.RenderThreadMs = CyclesToMs(GRenderThreadTime);
	Sample.RHIThreadMs = CyclesToMs(GRHIThreadTime);
	Sample.GpuMs = CyclesToMs(RHIGetGPUFrameCycles(0));

	if (NowSec - LastMemorySampleSec >= MemorySampleIntervalSec)
	{
		LastMemorySampleSec = NowSec;
		CachedProcessPhysMB = float(FPlatformMemory::GetStats().UsedPhysical / STS::MegaByte);

		FTextureMemoryStats TextureStats;
		RHIGetTextureMemoryStats(TextureStats);
		CachedRHITextureMB = float((TextureStats.StreamingMemorySize + TextureStats.NonStreamingMemorySize) / STS::MegaByte);
	}
	Sample.ProcessPhysMB = CachedProcessPhysMB;
	Sample.RHITextureMB = CachedRHITextureMB;

	Sample.TilesAlive = Tiles.Num();
	for (int32 Bucket = 0; Bucket < STS::NumBuckets; ++Bucket)
	{
		Sample.TexMB[Bucket] = float(TileCountsPerBucket[Bucket] * GetBucketChainMB(Bucket));
	}

	FillUpdateColumns(Sample);

	AccumulateHudStats(NowSec, Sample);
	if (Recorder.IsRecording())
	{
		Recorder.AddSample(NowSec, Sample);
	}
}

void ASTSTestbench::AccumulateHudStats(double NowSec, const FSTSFrameSample& Sample)
{
	++HudStats.Frames;
	HudStats.SumFrameMs += Sample.FrameMs;
	HudStats.SumGameMs += Sample.GameThreadMs;
	HudStats.SumRenderMs += Sample.RenderThreadMs;
	HudStats.SumRHIMs += Sample.RHIThreadMs;
	HudStats.SumGpuMs += Sample.GpuMs;
	HudStats.MaxFrameMs = FMath::Max(HudStats.MaxFrameMs, Sample.FrameMs);
	HudStats.MaxGameMs = FMath::Max(HudStats.MaxGameMs, Sample.GameThreadMs);
	HudStats.MaxRenderMs = FMath::Max(HudStats.MaxRenderMs, Sample.RenderThreadMs);
	HudStats.MaxRHIMs = FMath::Max(HudStats.MaxRHIMs, Sample.RHIThreadMs);
	HudStats.MaxGpuMs = FMath::Max(HudStats.MaxGpuMs, Sample.GpuMs);

	HudStats.Issued += Sample.RequestsIssued;
	HudStats.Released += Sample.ResponsesReleased;
	HudStats.ReleasedMB += Sample.ResponseMB;
	HudStats.LatencySumMs += double(Sample.ResponseLatencyAvgMs) * Sample.ResponsesReleased;
	HudStats.LatencyMaxMs = FMath::Max(HudStats.LatencyMaxMs, Sample.ResponseLatencyMaxMs);
	HudStats.ConsumerSumMs += Sample.ConsumerMs;
	HudStats.ConsumerMaxMs = FMath::Max(HudStats.ConsumerMaxMs, Sample.ConsumerMs);

	const double WindowSec = NowSec - HudStats.WindowStartSec;
	if (WindowSec < HudWindowSec)
	{
		return;
	}

	const double InvFrames = 1.0 / HudStats.Frames;
	HudStats.IssuedPerSec = float(HudStats.Issued / WindowSec);
	HudStats.ReleasedPerSec = float(HudStats.Released / WindowSec);
	HudStats.MBPerSec = float(HudStats.ReleasedMB / WindowSec);
	HudStats.LatencyAvgMs = HudStats.Released > 0 ? float(HudStats.LatencySumMs / HudStats.Released) : 0.0f;
	HudStats.ShownLatencyMaxMs = HudStats.LatencyMaxMs;
	HudStats.ConsumerAvgMs = float(HudStats.ConsumerSumMs * InvFrames);
	HudStats.ShownConsumerMaxMs = HudStats.ConsumerMaxMs;
	HudStats.Issued = HudStats.Released = 0;
	HudStats.ReleasedMB = HudStats.LatencySumMs = HudStats.ConsumerSumMs = 0.0;
	HudStats.LatencyMaxMs = HudStats.ConsumerMaxMs = 0.0f;

	HudStats.AvgFrameMs = float(HudStats.SumFrameMs * InvFrames);
	HudStats.Fps = HudStats.AvgFrameMs > 0.0f ? 1000.0f / HudStats.AvgFrameMs : 0.0f;
	HudStats.AvgGameMs = float(HudStats.SumGameMs * InvFrames);
	HudStats.AvgRenderMs = float(HudStats.SumRenderMs * InvFrames);
	HudStats.AvgRHIMs = float(HudStats.SumRHIMs * InvFrames);
	HudStats.AvgGpuMs = float(HudStats.SumGpuMs * InvFrames);
	HudStats.ShownMaxFrameMs = HudStats.MaxFrameMs;
	HudStats.ShownMaxGameMs = HudStats.MaxGameMs;
	HudStats.ShownMaxRenderMs = HudStats.MaxRenderMs;
	HudStats.ShownMaxRHIMs = HudStats.MaxRHIMs;
	HudStats.ShownMaxGpuMs = HudStats.MaxGpuMs;

	HudStats.WindowStartSec = NowSec;
	HudStats.Frames = 0;
	HudStats.SumFrameMs = HudStats.SumGameMs = HudStats.SumRenderMs = HudStats.SumRHIMs = HudStats.SumGpuMs = 0.0;
	HudStats.MaxFrameMs = HudStats.MaxGameMs = HudStats.MaxRenderMs = HudStats.MaxRHIMs = HudStats.MaxGpuMs = 0.0f;
}

void ASTSTestbench::RunPhase0Sequence()
{
	if (!IsGameWorldValid(TEXT("RunPhase0Sequence")))
	{
		return;
	}
	if (SequenceStep != ESTSSequenceStep::Idle)
	{
		UE_LOG(LogSTS, Warning, TEXT("RunPhase0Sequence ignored: the sequence is already running."));
		return;
	}
	if (bScenarioRunning)
	{
		UE_LOG(LogSTS, Warning, TEXT("RunPhase0Sequence ignored: a scenario is running."));
		return;
	}
	if (Recorder.IsRecording())
	{
		UE_LOG(LogSTS, Warning, TEXT("RunPhase0Sequence ignored: stop the active recording first."));
		return;
	}

	bAutoRecordPending = false;
	if (!Tiles.IsEmpty() || bCreationInProgress)
	{
		UE_LOG(LogSTS, Log, TEXT("Sequence: destroying the existing tiles so it starts from an empty scene."));
		DestroyTiles();
	}

	// Meshes and payloads are prepared now so the measured creation frame excludes their generation.
	ResolveBaseMaterial();
	EnsureMeshes();
	TSet<int32> Needed;
	for (const int32 Resolution : STS::BucketResolutions)
	{
		Needed.Add(Resolution);
	}
	if (STS::ResolutionToBucket(RemainderResolution) != INDEX_NONE)
	{
		Needed.Add(RemainderResolution);
	}
	EnsurePayloadLibrary(Needed);

	BeginSequenceStep(ESTSSequenceStep::Warmup, FPlatformTime::Seconds());
}

void ASTSTestbench::BeginSequenceStep(ESTSSequenceStep Step, double NowSec)
{
	SequenceStep = Step;
	SequenceStepStartSec = NowSec;
	UE_LOG(LogSTS, Log, TEXT("Sequence: %s"), GetSequenceStepName(Step));
	TRACE_BOOKMARK(TEXT("STS Sequence: %s"), GetSequenceStepName(Step));
}

void ASTSTestbench::UpdateSequence(double NowSec)
{
	const double StepElapsedSec = NowSec - SequenceStepStartSec;
	const double RecordSec = FMath::Max(1.0f, SeqRecordSec);

	switch (SequenceStep)
	{
	case ESTSSequenceStep::Idle:
		break;

	case ESTSSequenceStep::Warmup:
		if (StepElapsedSec >= SeqWarmupSec)
		{
			StartRecordingNamed(TEXT("Empty"), RecordSec);
			BeginSequenceStep(ESTSSequenceStep::RecordEmpty, NowSec);
		}
		break;

	case ESTSSequenceStep::RecordEmpty:
		if (!Recorder.IsRecording())
		{
			CreateTiles();
			BeginSequenceStep(ESTSSequenceStep::WaitForCreate, NowSec);
		}
		break;

	case ESTSSequenceStep::WaitForCreate:
		if (!bCreationInProgress)
		{
			BeginSequenceStep(ESTSSequenceStep::SettleTiles, NowSec);
		}
		break;

	case ESTSSequenceStep::SettleTiles:
		if (StepElapsedSec >= SeqSettleSec)
		{
			StartRecordingNamed(TEXT("Tiles"), RecordSec);
			BeginSequenceStep(ESTSSequenceStep::RecordTiles, NowSec);
		}
		break;

	case ESTSSequenceStep::RecordTiles:
		if (!Recorder.IsRecording())
		{
			if (bSeqIncludeDestroy)
			{
				DestroyTiles();
				BeginSequenceStep(ESTSSequenceStep::SettleDestroy, FPlatformTime::Seconds());
			}
			else
			{
				FinishSequence();
			}
		}
		break;

	case ESTSSequenceStep::SettleDestroy:
		if (StepElapsedSec >= SeqSettleSec)
		{
			StartRecordingNamed(TEXT("AfterDestroy"), RecordSec);
			BeginSequenceStep(ESTSSequenceStep::RecordAfterDestroy, NowSec);
		}
		break;

	case ESTSSequenceStep::RecordAfterDestroy:
		if (!Recorder.IsRecording())
		{
			FinishSequence();
		}
		break;
	}
}

void ASTSTestbench::FinishSequence()
{
	SequenceStep = ESTSSequenceStep::Idle;
	UE_LOG(LogSTS, Log, TEXT("Sequence: complete. Output is in %s"), *FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SimTexStrm")));
	if (bSequenceQuitWhenDone)
	{
		UE_LOG(LogSTS, Log, TEXT("Sequence: quitting."));
		FPlatformMisc::RequestExit(false, TEXT("STS Phase 0 sequence complete"));
	}
}

// --- Summary and render settings ---

bool ASTSTestbench::GatherRenderSettings(TArray<FString>& OutLines, FString& OutProfileSummary) const
{
	IConsoleManager& ConsoleManager = IConsoleManager::Get();
	const auto ReadCVar = [&ConsoleManager, &OutLines](const TCHAR* Name) -> int32
	{
		IConsoleVariable* CVar = ConsoleManager.FindConsoleVariable(Name, false);
		if (!CVar)
		{
			OutLines.Add(FString::Printf(TEXT("%s = <not found>"), Name));
			return INDEX_NONE;
		}
		OutLines.Add(FString::Printf(TEXT("%s = %s (set by %s)"), Name, *CVar->GetString(), GetConsoleVariableSetByName(CVar->GetFlags())));
		return CVar->GetInt();
	};

	const int32 RayTracingCVar = ReadCVar(TEXT("r.RayTracing"));
	const int32 GlobalIllumination = ReadCVar(TEXT("r.DynamicGlobalIlluminationMethod"));
	const int32 Reflections = ReadCVar(TEXT("r.ReflectionMethod"));
	const int32 VirtualShadowMaps = ReadCVar(TEXT("r.Shadow.Virtual.Enable"));
	const int32 VSync = ReadCVar(TEXT("r.VSync"));
	const int32 MaxFps = ReadCVar(TEXT("t.MaxFPS"));
	ReadCVar(TEXT("r.ScreenPercentage"));

	const bool bRayTracingAllowed = GIsRHIInitialized && IsRayTracingAllowed();
	OutLines.Add(FString::Printf(TEXT("IsRayTracingAllowed() = %s"), bRayTracingAllowed ? TEXT("true") : TEXT("false")));

	TArray<FString> Problems;
	if (bRayTracingAllowed || RayTracingCVar > 0)
	{
		Problems.Add(TEXT("ray tracing on"));
	}
	if (GlobalIllumination != 0)
	{
		Problems.Add(GlobalIllumination == 1 ? TEXT("Lumen GI") : TEXT("GI method not 0"));
	}
	if (Reflections != 0 && Reflections != 2)
	{
		Problems.Add(TEXT("Lumen reflections"));
	}
	if (VirtualShadowMaps != 0)
	{
		Problems.Add(TEXT("virtual shadow maps"));
	}

	const bool bProfileApplied = Problems.IsEmpty();
	OutLines.Add(bProfileApplied
		? FString(TEXT("Benchmark render profile: APPLIED"))
		: FString::Printf(TEXT("Benchmark render profile: NOT APPLIED (%s)"), *FString::Join(Problems, TEXT(", "))));

	OutProfileSummary = FString::Printf(TEXT("RT %s | GI %d | Refl %d | VSM %d | VSync %d | MaxFPS %d | %s"),
		bRayTracingAllowed ? TEXT("on") : TEXT("off"), GlobalIllumination, Reflections, VirtualShadowMaps, VSync, MaxFps,
		bProfileApplied ? TEXT("benchmark profile OK") : TEXT("NOT the benchmark profile"));
	return bProfileApplied;
}

void ASTSTestbench::LogRenderSettings() const
{
	TArray<FString> Lines;
	FString ProfileSummary;
	GatherRenderSettings(Lines, ProfileSummary);

	UE_LOG(LogSTS, Log, TEXT("Effective render settings at BeginPlay:"));
	for (const FString& Line : Lines)
	{
		UE_LOG(LogSTS, Log, TEXT("  %s"), *Line);
	}
}

void ASTSTestbench::GatherSummaryHeader(TArray<FString>& OutLines) const
{
	OutLines.Add(FString::Printf(TEXT("Build: %s%s"), LexToString(FApp::GetBuildConfiguration()), WITH_EDITOR ? TEXT(" (editor build)") : TEXT("")));
	OutLines.Add(FString::Printf(TEXT("RHI: %s, adapter: %s"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT("none"), *GRHIAdapterName));

	FIntPoint ViewportSize = FIntPoint::ZeroValue;
	if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		ViewportSize = GEngine->GameViewport->Viewport->GetSizeXY();
	}
	OutLines.Add(FString::Printf(TEXT("Resolution: %dx%d"), ViewportSize.X, ViewportSize.Y));
	OutLines.Add(FString::Printf(TEXT("Command line: %s"), FCommandLine::Get()));

	OutLines.Add(TEXT("Effective render settings (VSync / t.MaxFPS included):"));
	TArray<FString> RenderLines;
	FString ProfileSummary;
	GatherRenderSettings(RenderLines, ProfileSummary);
	for (const FString& Line : RenderLines)
	{
		OutLines.Add(FString::Printf(TEXT("  %s"), *Line));
	}

	double TotalMB = 0.0;
	FString PerResolution;
	for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
	{
		const double MB = TileCountsPerBucket[Bucket] * GetBucketChainMB(Bucket);
		TotalMB += MB;
		PerResolution += FString::Printf(TEXT("%s%d: %d tiles, %.1f MB"), PerResolution.IsEmpty() ? TEXT("") : TEXT("; "), STS::BucketResolutions[Bucket], TileCountsPerBucket[Bucket], MB);
	}
	OutLines.Add(FString::Printf(TEXT("Tiles alive at stop: %d (%s; total %.1f MB)"), Tiles.Num(), *PerResolution, TotalMB));
	OutLines.Add(FString::Printf(TEXT("Last tile creation: %.1f ms game-thread time (tiles per frame knob: %d)"), LastCreationMs, TilesCreatedPerFrame));

	GatherUpdateSummary(OutLines);

	OutLines.Add(TEXT("Timing sources:"));
	OutLines.Add(TEXT("  FrameMs: wall-clock delta between consecutive FCoreDelegates::OnEndFrame calls (no latency)."));
	OutLines.Add(TEXT("  GameThreadMs: GGameThreadTime, busy time excluding waits; set in FViewport::Draw, so it covers the frame just ended."));
	OutLines.Add(TEXT("  RenderThreadMs: GRenderThreadTime, busy time excluding idle; set after present on the render thread, lags about 1 frame."));
	OutLines.Add(TEXT("  RHIThreadMs: GRHIThreadTime, busy time excluding waits; lags about 1 frame, 0 without an RHI thread."));
	OutLines.Add(TEXT("  GpuMs: RHIGetGPUFrameCycles(0), timestamp-query history; lags 1-2 frames."));
	if (Window.Issued > 0 || Window.Released > 0)
	{
		OutLines.Add(TEXT("  ConsumerMs, ChecksumMs, IssueMs: FPlatformTime deltas on the game thread inside Tick (drain + validate + free, checksum only, trigger selection + submit); per frame, no latency."));
	}
}

// --- HUD ---

void ASTSTestbench::DrawHud(UCanvas* Canvas, APlayerController* /*Controller*/)
{
	if (!bShowHud || !Canvas || !GEngine)
	{
		return;
	}

	const double NowSec = FPlatformTime::Seconds();

	TArray<TPair<FString, FLinearColor>> Lines;
	Lines.Emplace(FString::Printf(TEXT("STS Phase 1 | %.1f fps | frame %.2f ms (max %.2f)"), HudStats.Fps, HudStats.AvgFrameMs, HudStats.ShownMaxFrameMs), FLinearColor::White);
	Lines.Emplace(FString::Printf(TEXT("Game %.2f (%.2f) | Render %.2f (%.2f) | RHI %.2f (%.2f) | GPU %.2f (%.2f) ms, avg (max)"),
		HudStats.AvgGameMs, HudStats.ShownMaxGameMs, HudStats.AvgRenderMs, HudStats.ShownMaxRenderMs, HudStats.AvgRHIMs, HudStats.ShownMaxRHIMs, HudStats.AvgGpuMs, HudStats.ShownMaxGpuMs), FLinearColor::White);

	FString TileLine = FString::Printf(TEXT("Tiles alive: %d"), Tiles.Num());
	for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
	{
		TileLine += FString::Printf(TEXT(" | %d: %d (%.0f MB)"), STS::BucketResolutions[Bucket], TileCountsPerBucket[Bucket], TileCountsPerBucket[Bucket] * GetBucketChainMB(Bucket));
	}
	if (bCreationInProgress)
	{
		TileLine += FString::Printf(TEXT(" | creating %d/%d"), NextTileToCreate, TilePlans.Num());
	}
	Lines.Emplace(TileLine, FLinearColor::White);
	Lines.Emplace(FString::Printf(TEXT("RAM %.0f MB | RHI textures %.0f MB | last tile creation %.0f ms"), CachedProcessPhysMB, CachedRHITextureMB, LastCreationMs), FLinearColor::White);

	if (Recorder.IsRecording())
	{
		const FString Duration = Recorder.GetDurationSec() > 0.0 ? FString::Printf(TEXT(" / %.0f s"), Recorder.GetDurationSec()) : FString();
		Lines.Emplace(FString::Printf(TEXT("REC '%s' %.1f s%s (%llu frames)"), *Recorder.GetName(), Recorder.GetElapsedSec(NowSec), *Duration, Recorder.GetFrameCount()), FLinearColor::Red);
	}
	else
	{
		Lines.Emplace(TEXT("Recorder idle"), FLinearColor::Gray);
	}

	if (SequenceStep != ESTSSequenceStep::Idle)
	{
		Lines.Emplace(FString::Printf(TEXT("Sequence: %s (%.1f s)"), GetSequenceStepName(SequenceStep), NowSec - SequenceStepStartSec), FLinearColor::Yellow);
	}

	AppendUpdateHud(Lines, NowSec);

	if (NowSec - LastProfileRefreshSec >= 1.0)
	{
		LastProfileRefreshSec = NowSec;
		TArray<FString> RenderLines;
		bBenchmarkProfileApplied = GatherRenderSettings(RenderLines, RenderProfileSummary);
	}
	Lines.Emplace(RenderProfileSummary, bBenchmarkProfileApplied ? FLinearColor::Green : FLinearColor::Yellow);

	if (!MaterialWarning.IsEmpty())
	{
		Lines.Emplace(FString::Printf(TEXT("WARNING: %s"), *MaterialWarning), FLinearColor::Red);
	}

	const UFont* Font = GEngine->GetSmallFont();
	const float Scale = FMath::Max(1.0f, Canvas->ClipY / 1080.0f);
	const float LineHeight = Font->GetMaxCharHeight() * Scale;
	const float Margin = 8.0f * Scale;

	float MaxWidth = 0.0f;
	for (const TPair<FString, FLinearColor>& Line : Lines)
	{
		float Width = 0.0f;
		float Height = 0.0f;
		Canvas->TextSize(Font, Line.Key, Width, Height, Scale, Scale);
		MaxWidth = FMath::Max(MaxWidth, Width);
	}

	FCanvasTileItem Background(FVector2D(Margin * 0.5f, Margin * 0.5f), FVector2D(MaxWidth + Margin, LineHeight * Lines.Num() + Margin), FLinearColor(0.0f, 0.0f, 0.0f, 0.6f));
	Background.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Background);

	float Y = Margin;
	for (const TPair<FString, FLinearColor>& Line : Lines)
	{
		Canvas->SetDrawColor(Line.Value.ToFColor(true));
		Canvas->DrawText(Font, Line.Key, Margin, Y, Scale, Scale);
		Y += LineHeight;
	}
}

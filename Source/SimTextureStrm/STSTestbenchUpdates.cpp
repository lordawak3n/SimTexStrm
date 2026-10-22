// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSTestbench.h"

#include "SimTextureStrm.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Hash/CityHash.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/MiscTrace.h"

namespace
{
	constexpr int32 MaxLoggedChecksumFailures = 8;
	constexpr int32 MaxBurstLinesInSummary = 24;
	constexpr int32 MaxVariants = 16;
	constexpr int32 MinResolution = 512;
	constexpr int32 MaxResolution = 4096;

	int32 PositiveMod(int32 Value, int32 Divisor)
	{
		return ((Value % Divisor) + Divisor) % Divisor;
	}

	double ElapsedMsSince(double StartSec)
	{
		return (FPlatformTime::Seconds() - StartSec) * 1000.0;
	}

	FString StatsLine(const TCHAR* Label, const TArray<float>& Values)
	{
		const FSTSColumnStats Stats = FSTSRecorder::ComputeStats(Values);
		return FString::Printf(TEXT("  %-30s avg %.3f | p50 %.3f | p99 %.3f | max %.3f (n=%d)"), Label, Stats.Average, Stats.P50, Stats.P99, Stats.Max, Stats.Count);
	}
}

// --- Server and payload library ---

bool ASTSTestbench::EnsureServer()
{
	const int32 Workers = FMath::Clamp(ServerWorkerCount, 1, 32);
	if (Server && Server->GetWorkerCount() != Workers && Server->GetInFlight() == 0)
	{
		Server.Reset();
	}
	if (!Server)
	{
		Server = MakeUnique<FSTSTerrainServer>(Workers);
		Server->SetEpoch(TileEpoch);
		LastServerCounters = FSTSServerCounters();
		UE_LOG(LogSTS, Log, TEXT("Terrain server started with %d worker threads."), Workers);
	}
	ApplyServerSettings();
	return true;
}

void ASTSTestbench::ShutdownServer()
{
	if (Server)
	{
		const int32 InFlight = Server->GetInFlight();
		Server->Shutdown();
		Server.Reset();
		UE_LOG(LogSTS, Log, TEXT("Terrain server stopped (%d requests were still in flight and were dropped)."), InFlight);
	}
	ServerLibrary.Reset();
	ServerLibraryLooks.Empty();
	ServerLibraryVariants = 0;
}

void ASTSTestbench::ApplyServerSettings()
{
	if (!Server)
	{
		return;
	}
	FSTSServerSettings Settings;
	Settings.LatencyMs = ServerLatencyMs;
	Settings.LatencyJitterMs = ServerLatencyJitterMs;
	Settings.LinkBandwidthMBps = ServerLinkBandwidthMBps;
	Settings.MaxInFlight = ServerMaxInFlight;
	Settings.CorruptEveryN = DebugCorruptEveryN;
	Server->SetSettings(Settings);
	Server->SetBufferPool(ActiveUploadMode == ESTSUploadMode::Regions ? BufferPool : nullptr);
}

bool ASTSTestbench::IsServerIdle() const
{
	return (!Server || Server->GetInFlight() == 0) && IsUploadPathIdle();
}

bool ASTSTestbench::EnsureServerLibrary()
{
	const int32 Variants = FMath::Clamp(VariantsPerResolution, 1, MaxVariants);
	TArray<FSTSTextureLook> Looks;
	for (const int32 Resolution : STS::BucketResolutions)
	{
		Looks.Add(GetLookForResolution(Resolution));
	}
	if (ServerLibrary && ServerLibraryVariants == Variants && ServerLibraryLooks == Looks)
	{
		return true;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(STS_BuildPayloadLibrary);
	const double StartSec = FPlatformTime::Seconds();

	TSharedRef<FSTSPayloadLibrary> Library = MakeShared<FSTSPayloadLibrary>();
	for (int32 Bucket = 0; Bucket < STS::NumBuckets; ++Bucket)
	{
		const int32 Resolution = STS::BucketResolutions[Bucket];
		for (int32 Variant = 0; Variant < Variants; ++Variant)
		{
			// The Phase 0 library already holds one variant of each resolution; reuse it when it matches.
			const TSharedPtr<const FSTSMipChain>* Existing = PayloadLibrary.Find(Resolution);
			const FSTSTextureLook* ExistingLook = PayloadLooks.Find(Resolution);
			if (Existing && Existing->IsValid() && ExistingLook && *ExistingLook == Looks[Bucket] && PayloadLibraryVariant == Variant)
			{
				Library->Add(FSTSPayload::FromChain(Resolution, Variant, **Existing));
				continue;
			}

			FSTSMipChain Chain;
			FSTSDxt1Generator::GenerateChain(Resolution, Resolution, Looks[Bucket], uint32(Variant), Chain);
			Library->Add(FSTSPayload::FromChain(Resolution, Variant, Chain));
		}
	}

	ServerLibrary = Library;
	ServerLibraryLooks = MoveTemp(Looks);
	ServerLibraryVariants = Variants;
	UE_LOG(LogSTS, Log, TEXT("Built the server payload library: %d entries (%d variants x %d resolutions), %.1f MB in %.1f ms."),
		Library->Num(), Variants, STS::NumBuckets, Library->GetTotalBytes() / STS::MegaByte, ElapsedMsSince(StartSec));
	return true;
}

void ASTSTestbench::ResetLogicalState()
{
	FMemory::Memzero(LogicalCountsPerBucket);
	PatchTriggerCount = 0;

	// Responses of the old epoch are discarded and counted as stale when the server next drains.
	++TileEpoch;
	if (Builder)
	{
		Builder->SetEpoch(TileEpoch);
	}
	if (Server)
	{
		Server->SetEpoch(TileEpoch);
		UE_LOG(LogSTS, Log, TEXT("Tile epoch is now %u; %d requests in flight will be discarded as stale."), TileEpoch, Server->GetInFlight());
	}
}

void ASTSTestbench::ResetTriggerState()
{
	PatchTriggerCount = 0;
	Spread = FTimeline();
	BurstRepeat = FTimeline();
}

// --- Buttons and entry points ---

void ASTSTestbench::TriggerBurst()
{
	TriggerBurstCounts(Burst4k, Burst2k, Burst1k, Burst512);
}

void ASTSTestbench::TriggerBurstCounts(int32 Requested4k, int32 Requested2k, int32 Requested1k, int32 Requested512)
{
	if (!IsGameWorldValid(TEXT("TriggerBurst")))
	{
		return;
	}
	if (Tiles.IsEmpty())
	{
		UE_LOG(LogSTS, Warning, TEXT("TriggerBurst ignored: there are no tiles."));
		return;
	}

	EnsureServer();
	EnsureServerLibrary();

	const int32 Counts[STS::NumBuckets] = { FMath::Max(0, Requested512), FMath::Max(0, Requested1k), FMath::Max(0, Requested2k), FMath::Max(0, Requested4k) };
	IssueBurst(Counts, FPlatformTime::Seconds(), TEXT("burst"));
}

void ASTSTestbench::StartSpread()
{
	StartSpreadRate(SpreadRatePerSec, SpreadDurationSec);
}

void ASTSTestbench::StartSpreadRate(double RatePerSec, double DurationSec)
{
	if (!IsGameWorldValid(TEXT("StartSpread")))
	{
		return;
	}
	if (RatePerSec <= 0.0)
	{
		UE_LOG(LogSTS, Warning, TEXT("StartSpread ignored: the rate must be above 0."));
		return;
	}
	if (Spread.bActive)
	{
		UE_LOG(LogSTS, Warning, TEXT("StartSpread ignored: a spread is already running. Stop it first."));
		return;
	}

	EnsureServer();
	EnsureServerLibrary();

	Spread = FTimeline();
	Spread.bActive = true;
	Spread.StartSec = FPlatformTime::Seconds();
	Spread.Rate = RatePerSec;
	Spread.DurationSec = FMath::Max(0.0, DurationSec);
	UE_LOG(LogSTS, Log, TEXT("Spread started: %.2f requests/s for %s."), RatePerSec, Spread.DurationSec > 0.0 ? *FString::Printf(TEXT("%.1f s"), Spread.DurationSec) : TEXT("until stopped"));
	TRACE_BOOKMARK(TEXT("STS Spread Start: %.1f/s"), RatePerSec);
}

void ASTSTestbench::StopSpread()
{
	if (!Spread.bActive && !BurstRepeat.bActive)
	{
		UE_LOG(LogSTS, Log, TEXT("StopSpread: nothing is running."));
		return;
	}
	if (Spread.bActive)
	{
		UE_LOG(LogSTS, Log, TEXT("Spread stopped after %lld requests due."), Spread.NextIndex);
	}
	if (BurstRepeat.bActive)
	{
		UE_LOG(LogSTS, Log, TEXT("Burst repeat stopped after %lld bursts."), BurstRepeat.NextIndex);
	}
	Spread.bActive = false;
	BurstRepeat.bActive = false;
	TRACE_BOOKMARK(TEXT("STS Spread Stop"));
}

void ASTSTestbench::SetPatternByName(const FString& Name)
{
	const int64 Value = StaticEnum<ESTSUpdatePattern>()->GetValueByNameString(Name);
	if (Value == INDEX_NONE)
	{
		UE_LOG(LogSTS, Error, TEXT("'%s' is not an update pattern (SameResolution, StepUp, StepDown, Mixed, Oscillate)."), *Name);
		return;
	}
	UpdatePattern = ESTSUpdatePattern(Value);
}

void ASTSTestbench::SetScenarioPresetByName(const FString& Name)
{
	const int64 Value = StaticEnum<ESTSScenarioPreset>()->GetValueByNameString(Name);
	if (Value == INDEX_NONE)
	{
		UE_LOG(LogSTS, Error, TEXT("'%s' is not a scenario preset."), *Name);
		return;
	}
	ScenarioPreset = ESTSScenarioPreset(Value);
	ApplyScenarioPreset();
}

// --- Triggers ---

void ASTSTestbench::UpdateTimelines(double NowSec)
{
	for (int32 Which = 0; Which < 2; ++Which)
	{
		FTimeline& Timeline = Which == 0 ? Spread : BurstRepeat;
		if (!Timeline.bActive)
		{
			continue;
		}

		// Item k is due at StartSec + k / Rate. A small epsilon keeps 10/s over 10 s at exactly 100 items.
		const int64 Total = Timeline.DurationSec > 0.0 ? int64(FMath::CeilToDouble(Timeline.DurationSec * Timeline.Rate - 1.0e-6)) : MAX_int64;
		const int64 DueEnd = FMath::Min<int64>(Total, int64(FMath::FloorToDouble((NowSec - Timeline.StartSec) * Timeline.Rate + 1.0e-9)) + 1);

		if (DueEnd > Timeline.NextIndex)
		{
			if (Which == 0)
			{
				TArray<TPair<int32, double>> Targets;
				Targets.Reserve(int32(DueEnd - Timeline.NextIndex));
				for (int64 Index = Timeline.NextIndex; Index < DueEnd; ++Index)
				{
					Targets.Emplace(PickSpreadResolution(), Timeline.StartSec + double(Index) / Timeline.Rate);
				}
				IssueBatch(Targets, false, TEXT("spread"));
			}
			else
			{
				for (int64 Index = Timeline.NextIndex; Index < DueEnd; ++Index)
				{
					IssueBurst(Timeline.BurstCounts, Timeline.StartSec + double(Index) / Timeline.Rate, TEXT("burst repeat"));
				}
			}
			Timeline.NextIndex = DueEnd;
		}

		if (Timeline.NextIndex >= Total)
		{
			Timeline.bActive = false;
			UE_LOG(LogSTS, Log, TEXT("%s finished: %lld items due over %.1f s."), Which == 0 ? TEXT("Spread") : TEXT("Burst repeat"), Timeline.NextIndex, Timeline.DurationSec);
			TRACE_BOOKMARK(TEXT("STS Spread Stop"));
		}
	}
}

int32 ASTSTestbench::PickSpreadResolution()
{
	const float Weights[STS::NumBuckets] = { SpreadWeight512, SpreadWeight1k, SpreadWeight2k, SpreadWeight4k };
	float Total = 0.0f;
	for (const float Weight : Weights)
	{
		Total += FMath::Max(0.0f, Weight);
	}

	if (Total <= 0.0f)
	{
		return STS::BucketResolutions[UpdateStream.RandHelper(STS::NumBuckets)];
	}

	float Remaining = UpdateStream.FRand() * Total;
	int32 Chosen = 0;
	for (int32 Bucket = 0; Bucket < STS::NumBuckets; ++Bucket)
	{
		const float Weight = FMath::Max(0.0f, Weights[Bucket]);
		if (Weight <= 0.0f)
		{
			continue;
		}
		Chosen = Bucket;
		if (Remaining < Weight)
		{
			break;
		}
		Remaining -= Weight;
	}
	return STS::BucketResolutions[Chosen];
}

void ASTSTestbench::IssueBurst(const int32* Counts, double NowSec, const TCHAR* Source)
{
	TArray<TPair<int32, double>> Targets;
	for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
	{
		for (int32 Index = 0; Index < Counts[Bucket]; ++Index)
		{
			Targets.Emplace(STS::BucketResolutions[Bucket], NowSec);
		}
	}
	IssueBatch(Targets, true, Source);
}

bool ASTSTestbench::IsTileSelectable(int32 TileIndex, const FSelectionContext& Context) const
{
	if (Context.Selected[TileIndex])
	{
		return false;
	}

	const FSTSTile& Tile = Tiles[TileIndex];
	if (Tile.PendingRequestId != INDEX_NONE && !bAllowRetargetInFlight)
	{
		return false;
	}

	switch (TargetMode)
	{
	case ESTSTargetMode::RankRange:
		return Tile.Rank >= RankMin && Tile.Rank <= RankMax;
	case ESTSTargetMode::Patch:
		return PositiveMod(Tile.CellX - Context.PatchMinX, ActiveCellsX) < PatchSizeX
			&& PositiveMod(Tile.CellY - Context.PatchMinY, ActiveCellsY) < PatchSizeY;
	default:
		return true;
	}
}

void ASTSTestbench::CollectPool(ESTSUpdatePattern Pattern, int32 TargetResolution, const FSelectionContext& Context, TArray<int32>& OutPool) const
{
	OutPool.Reset();
	const int32 PairHigh = FMath::Max(TargetResolution, 2 * MinResolution);
	for (int32 TileIndex = 0; TileIndex < Tiles.Num(); ++TileIndex)
	{
		const int32 Logical = Tiles[TileIndex].LogicalResolution;
		bool bMatches = false;
		switch (Pattern)
		{
		case ESTSUpdatePattern::SameResolution:
			bMatches = Logical == TargetResolution;
			break;
		case ESTSUpdatePattern::StepUp:
			bMatches = Logical * 2 == TargetResolution;
			break;
		case ESTSUpdatePattern::StepDown:
			bMatches = Logical == TargetResolution * 2;
			break;
		case ESTSUpdatePattern::Oscillate:
			bMatches = Logical == PairHigh || Logical * 2 == PairHigh;
			break;
		default:
			break;
		}
		if (bMatches && IsTileSelectable(TileIndex, Context))
		{
			OutPool.Add(TileIndex);
		}
	}
}

int32 ASTSTestbench::NextSameResolutionVariant(int32 Variant) const
{
	const int32 Variants = FMath::Clamp(VariantsPerResolution, 1, MaxVariants);
	int32 Next = (Variant + 1) % Variants;
	if (Next == 0 && bSkipVariantZero && Variants > 2)
	{
		Next = 1;
	}
	return Next;
}

ASTSTestbench::EPickResult ASTSTestbench::PickTile(int32 TargetResolution, FSelectionContext& Context, int32& OutTile, int32& OutResolution, int32& OutVariant)
{
	const int32 Variants = FMath::Clamp(VariantsPerResolution, 1, MaxVariants);
	ESTSUpdatePattern Concrete = UpdatePattern;
	TArray<int32>* Pool = nullptr;

	const auto Collect = [this, &Context, TargetResolution](ESTSUpdatePattern Pattern, int32 Slot)
	{
		CollectPool(Pattern, TargetResolution, Context, PoolScratch[Slot]);
		return !PoolScratch[Slot].IsEmpty();
	};

	if (UpdatePattern == ESTSUpdatePattern::Mixed)
	{
		const ESTSUpdatePattern Options[3] = { ESTSUpdatePattern::SameResolution, ESTSUpdatePattern::StepUp, ESTSUpdatePattern::StepDown };
		const float Weights[3] = { MixedWeightSame, MixedWeightUp, MixedWeightDown };
		bool bHasPool[3] = {};
		float Total = 0.0f;
		int32 NumPools = 0;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			bHasPool[Index] = Collect(Options[Index], Index);
			if (bHasPool[Index])
			{
				Total += FMath::Max(0.0f, Weights[Index]);
				++NumPools;
			}
		}

		if (NumPools > 0)
		{
			int32 Chosen = INDEX_NONE;
			if (Total > 0.0f)
			{
				float Remaining = UpdateStream.FRand() * Total;
				for (int32 Index = 0; Index < 3; ++Index)
				{
					if (!bHasPool[Index] || Weights[Index] <= 0.0f)
					{
						continue;
					}
					Chosen = Index;
					if (Remaining < Weights[Index])
					{
						break;
					}
					Remaining -= Weights[Index];
				}
			}
			else
			{
				int32 Skip = UpdateStream.RandHelper(NumPools);
				for (int32 Index = 0; Index < 3; ++Index)
				{
					if (bHasPool[Index] && Skip-- == 0)
					{
						Chosen = Index;
						break;
					}
				}
			}
			Concrete = Options[Chosen];
			Pool = &PoolScratch[Chosen];
		}
	}
	else if (Collect(UpdatePattern, 0))
	{
		Pool = &PoolScratch[0];
	}

	if (!Pool)
	{
		if (ImpossibleStepPolicy == ESTSImpossibleStepPolicy::Skip)
		{
			return EPickResult::Skipped;
		}
		if (ImpossibleStepPolicy == ESTSImpossibleStepPolicy::FallbackToSameResolution && Collect(ESTSUpdatePattern::SameResolution, 0))
		{
			Concrete = ESTSUpdatePattern::SameResolution;
			Pool = &PoolScratch[0];
		}
		else
		{
			return EPickResult::Shortfall;
		}
	}

	OutTile = (*Pool)[UpdateStream.RandHelper(Pool->Num())];
	const FSTSTile& Tile = Tiles[OutTile];
	OutVariant = Tile.LogicalVariant % Variants;
	switch (Concrete)
	{
	case ESTSUpdatePattern::StepUp:
		OutResolution = FMath::Min(Tile.LogicalResolution * 2, MaxResolution);
		break;
	case ESTSUpdatePattern::StepDown:
		OutResolution = FMath::Max(Tile.LogicalResolution / 2, MinResolution);
		break;
	case ESTSUpdatePattern::Oscillate:
	{
		const int32 PairHigh = FMath::Max(TargetResolution, 2 * MinResolution);
		OutResolution = Tile.LogicalResolution == PairHigh ? PairHigh / 2 : PairHigh;
		break;
	}
	default:
		OutResolution = Tile.LogicalResolution;
		OutVariant = NextSameResolutionVariant(Tile.LogicalVariant);
		break;
	}
	return EPickResult::Picked;
}

void ASTSTestbench::IssueBatch(const TArray<TPair<int32, double>>& Targets, bool bBurst, const TCHAR* Source)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(STS_IssueBatch);
	const double StartSec = FPlatformTime::Seconds();

	FBatchResult Result;
	Result.TimeSec = StartSec;
	Result.Requested = Targets.Num();
	for (const TPair<int32, double>& Target : Targets)
	{
		const int32 Bucket = STS::ResolutionToBucket(Target.Key);
		if (Bucket != INDEX_NONE)
		{
			++Result.RequestedByBucket[Bucket];
		}
	}

	if (!Server || !ServerLibrary || Tiles.IsEmpty())
	{
		Result.Shortfall = Targets.Num();
	}
	else
	{
		if (bResetSeedPerTrigger)
		{
			UpdateStream.Initialize(UpdateSeed);
		}

		FSelectionContext Context;
		Context.Selected.SetNumZeroed(Tiles.Num());
		Context.PatchMinX = PatchCenterX + PatchStepX * PatchTriggerCount - PatchSizeX / 2;
		Context.PatchMinY = PatchCenterY + PatchStepY * PatchTriggerCount - PatchSizeY / 2;

		for (const TPair<int32, double>& Target : Targets)
		{
			int32 TileIndex = INDEX_NONE;
			int32 NewResolution = 0;
			int32 NewVariant = 0;
			EPickResult Pick;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(STS_TargetSelection);
				Pick = PickTile(Target.Key, Context, TileIndex, NewResolution, NewVariant);
			}

			if (Pick == EPickResult::Skipped)
			{
				++Result.Skipped;
				continue;
			}
			const TSharedPtr<const FSTSPayload> Payload = Pick == EPickResult::Picked ? ServerLibrary->Find(NewResolution, NewVariant) : nullptr;
			if (!Payload)
			{
				++Result.Shortfall;
				continue;
			}

			FSTSTile& Tile = Tiles[TileIndex];
			FSTSRequest Request;
			Request.RequestId = NextRequestId++;
			Request.TileIndex = TileIndex;
			Request.TileEpoch = TileEpoch;
			Request.Resolution = NewResolution;
			Request.Variant = NewVariant;
			Request.IssueFrame = GFrameCounter;
			Request.IssueTimeSec = Target.Value;
			Request.Priority = Tile.Rank;
			Server->Submit(Request, Payload.ToSharedRef(), Request.IssueTimeSec);

			Tile.PendingRequestId = Request.RequestId;
			Context.Selected[TileIndex] = 1;
			Context.SelectedList.Add(TileIndex);
			++Result.Issued;
			const int32 Bucket = STS::ResolutionToBucket(Target.Key);
			if (Bucket != INDEX_NONE)
			{
				++Result.IssuedByBucket[Bucket];
			}
		}

		Result.SelectionHash = CityHash64(reinterpret_cast<const char*>(Context.SelectedList.GetData()), uint32(Context.SelectedList.Num() * sizeof(int32)));
		++PatchTriggerCount;
	}

	Frame.Issued += Result.Issued;
	Frame.IssueMs += ElapsedMsSince(StartSec);
	TotalRequestsIssued += Result.Issued;

	if (bBurst)
	{
		NoteBurstIssued(NextRequestId - Result.Issued, NextRequestId);
		LastBurst = Result;
		bHasLastBurst = true;
		TRACE_BOOKMARK(TEXT("STS Burst: %d/%d"), Result.Issued, Result.Requested);
	}

	if (Recorder.IsRecording())
	{
		Window.SelectionHashChain = CityHash64WithSeed(reinterpret_cast<const char*>(&Result.SelectionHash), sizeof(uint64), Window.SelectionHashChain);
		if (bBurst)
		{
			++Window.BurstCount;
			Window.BurstRequested += Result.Requested;
			Window.BurstIssued += Result.Issued;
			if (Window.BurstLines.Num() < MaxBurstLinesInSummary)
			{
				Window.BurstLines.Add(FString::Printf(TEXT("  burst %d (%s): requested %d (4k %d, 2k %d, 1k %d, 512 %d), issued %d (4k %d, 2k %d, 1k %d, 512 %d), shortfall %d, skipped %d, selection hash %016llX"),
					Window.BurstCount, Source, Result.Requested, Result.RequestedByBucket[3], Result.RequestedByBucket[2], Result.RequestedByBucket[1], Result.RequestedByBucket[0],
					Result.Issued, Result.IssuedByBucket[3], Result.IssuedByBucket[2], Result.IssuedByBucket[1], Result.IssuedByBucket[0], Result.Shortfall, Result.Skipped, Result.SelectionHash));
			}
		}
		else
		{
			Window.SpreadRequested += Result.Requested;
			Window.SpreadIssued += Result.Issued;
		}
	}

	if (bBurst)
	{
		UE_LOG(LogSTS, Log, TEXT("Trigger (%s, frame %llu): requested %d (4k %d, 2k %d, 1k %d, 512 %d), issued %d, shortfall %d, skipped %d; pattern %s, target %s; selection hash %016llX over %d tiles."),
			Source, uint64(GFrameCounter), Result.Requested, Result.RequestedByBucket[3], Result.RequestedByBucket[2], Result.RequestedByBucket[1], Result.RequestedByBucket[0],
			Result.Issued, Result.Shortfall, Result.Skipped, *STS::EnumToString(UpdatePattern), *STS::EnumToString(TargetMode), Result.SelectionHash, Result.Issued);
	}
	else
	{
		UE_LOG(LogSTS, Verbose, TEXT("Trigger (%s, frame %llu): requested %d, issued %d, shortfall %d, skipped %d; selection hash %016llX."),
			Source, uint64(GFrameCounter), Result.Requested, Result.Issued, Result.Shortfall, Result.Skipped, Result.SelectionHash);
	}
}

// --- Consumer ---

void ASTSTestbench::ConsumeResponses(double NowSec)
{
	if (!Server)
	{
		return;
	}

	const double StartSec = FPlatformTime::Seconds();
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(STS_ConsumeResponses);
		ApplyServerSettings();

		ReleasedScratch.Reset();
		Server->Pump(NowSec, ReleasedScratch);

		const FSTSServerCounters& Counters = Server->GetCounters();
		const int32 NewStale = int32(Counters.StaleDiscarded - LastServerCounters.StaleDiscarded);
		Frame.Stale += NewStale;
		TotalStaleDiscarded += NewStale;
		if (Recorder.IsRecording())
		{
			Window.Late += Counters.Late - LastServerCounters.Late;
		}
		LastServerCounters = Counters;

		for (FSTSResponse& Response : ReleasedScratch)
		{
			ProcessResponse(Response, NowSec);
		}
		ReleasedScratch.Reset();
	}
	Frame.ConsumerMs += ElapsedMsSince(StartSec);
}

void ASTSTestbench::ProcessResponse(FSTSResponse& Response, double NowSec)
{
	const FSTSRequest& Request = Response.Request;
	const bool bRecording = Recorder.IsRecording();
	const bool bBaseline = STS::IsBaselineFamily(ActiveUploadMode);
	const bool bRegions = ActiveUploadMode == ESTSUploadMode::Regions;
	bool bSubmitBuild = false;
	bool bSubmitUpload = false;

	if (bVerifyChecksums)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(STS_ChecksumVerify);
		const double ChecksumStartSec = FPlatformTime::Seconds();
		const uint64 Computed = FSTSPayload::ComputeChecksum(Response.Buffer.GetData(), Response.Buffer.Num());
		const double ChecksumMs = ElapsedMsSince(ChecksumStartSec);

		Frame.ChecksumMs += ChecksumMs;
		TotalChecksumMs += ChecksumMs;
		TotalChecksumBytes += Response.Buffer.Num();
		if (bRecording)
		{
			Window.ChecksumMsTotal += ChecksumMs;
			Window.ChecksumMsWorst = FMath::Max(Window.ChecksumMsWorst, ChecksumMs);
			Window.ChecksumBytes += Response.Buffer.Num();
		}

		const bool bSizeMatches = Response.Source.IsValid() && Response.Buffer.Num() == Response.Source->Data.Num();
		if (Computed != Response.Checksum || !bSizeMatches)
		{
			++Frame.ChecksumFailures;
			++TotalChecksumFailures;
			if (LoggedChecksumFailures < MaxLoggedChecksumFailures)
			{
				++LoggedChecksumFailures;
				UE_LOG(LogSTS, Error, TEXT("Checksum mismatch: request %lld, tile %d, epoch %u, resolution %d, variant %d, expected %016llX, computed %016llX, %d bytes (expected %d)."),
					Request.RequestId, Request.TileIndex, Request.TileEpoch, Request.Resolution, Request.Variant, Response.Checksum, Computed,
					Response.Buffer.Num(), Response.Source.IsValid() ? Response.Source->Data.Num() : 0);
			}
		}
	}

	if (Tiles.IsValidIndex(Request.TileIndex))
	{
		FSTSTile& Tile = Tiles[Request.TileIndex];
		if (Tile.PendingRequestId == Request.RequestId)
		{
			const int32 OldBucket = STS::ResolutionToBucket(Tile.LogicalResolution);
			const int32 NewBucket = STS::ResolutionToBucket(Request.Resolution);
			if (OldBucket != INDEX_NONE)
			{
				--LogicalCountsPerBucket[OldBucket];
			}
			if (NewBucket != INDEX_NONE)
			{
				++LogicalCountsPerBucket[NewBucket];
			}
			Tile.LogicalResolution = Request.Resolution;
			Tile.LogicalVariant = Request.Variant;
			if (bBaseline || bRegions)
			{
				// The request stays pending until the apply step has put the texture on the tile.
				bSubmitBuild = bBaseline;
				bSubmitUpload = bRegions;
			}
			else
			{
				Tile.PendingRequestId = INDEX_NONE;
			}
		}
		else if (bRecording)
		{
			++Window.Superseded;
		}
	}

	const double LatencyMs = (NowSec - Request.IssueTimeSec) * 1000.0;
	++Frame.Released;
	Frame.ReleasedBytes += Response.Buffer.Num();
	Frame.LatencySumMs += LatencyMs;
	Frame.LatencyMaxMs = FMath::Max(Frame.LatencyMaxMs, LatencyMs);

	if (bRecording)
	{
		Window.LatencyMs.Add(float(LatencyMs));
		Window.ReleaseLagMs.Add(float((NowSec - Response.ReadyTimeSec) * 1000.0));
		Window.ModelLatencyMs.Add(float((Response.ReadyTimeSec - Request.IssueTimeSec) * 1000.0));
	}

	if (bSubmitBuild)
	{
		SubmitBuildJob(Response, NowSec);
	}
	else if (bSubmitUpload)
	{
		SubmitRegionsUpload(Response, NowSec);
	}
	else if (bRegions)
	{
		// Superseded or for a tile that is gone: nothing will upload it, so its buffer goes back to the pool.
		ReleaseResponseBuffer(Request, MoveTemp(Response.Buffer));
	}
}

void ASTSTestbench::FillUpdateColumns(FSTSFrameSample& Sample)
{
	Sample.PendingRequests = Server ? Server->GetInFlight() : 0;
	Sample.RequestsIssued = Frame.Issued;
	Sample.ResponsesReleased = Frame.Released;
	Sample.ResponseMB = float(Frame.ReleasedBytes / STS::MegaByte);
	Sample.ResponseLatencyAvgMs = Frame.Released > 0 ? float(Frame.LatencySumMs / Frame.Released) : 0.0f;
	Sample.ResponseLatencyMaxMs = float(Frame.LatencyMaxMs);
	Sample.StaleDiscarded = Frame.Stale;
	Sample.ChecksumFailures = Frame.ChecksumFailures;
	Sample.ConsumerMs = float(Frame.ConsumerMs);
	Sample.ChecksumMs = float(Frame.ChecksumMs);
	Sample.IssueMs = float(Frame.IssueMs);
	FillBaselineColumns(Sample);

	if (Recorder.IsRecording())
	{
		Window.Issued += Frame.Issued;
		Window.Released += Frame.Released;
		Window.Stale += Frame.Stale;
		Window.ChecksumFailures += Frame.ChecksumFailures;
		Window.ReleasedBytes += Frame.ReleasedBytes;
		Window.ConsumerMs.Add(Sample.ConsumerMs);
		Window.ChecksumMs.Add(Sample.ChecksumMs);
		Window.IssueMs.Add(Sample.IssueMs);
	}
	Frame = FUpdateFrameCounters();
}

// --- Summary and HUD ---

void ASTSTestbench::GatherUpdateSummary(TArray<FString>& OutLines) const
{
	if (Window.Issued == 0 && Window.Released == 0 && Window.Stale == 0 && Window.BurstCount == 0)
	{
		return;
	}

	OutLines.Add(FString());
	OutLines.Add(TEXT("Terrain server and update stream (Phase 1):"));
	OutLines.Add(FString::Printf(TEXT("  Server: %d workers, latency %.1f +/- %.1f ms, bandwidth %s, max in flight %s, %d variants per resolution, checksums %s%s"),
		Server ? Server->GetWorkerCount() : ServerWorkerCount, ServerLatencyMs, ServerLatencyJitterMs,
		ServerLinkBandwidthMBps > 0.0f ? *FString::Printf(TEXT("%.0f MB/s"), ServerLinkBandwidthMBps) : TEXT("unlimited"),
		ServerMaxInFlight > 0 ? *FString::FromInt(ServerMaxInFlight) : TEXT("unlimited"),
		FMath::Clamp(VariantsPerResolution, 1, MaxVariants), bVerifyChecksums ? TEXT("ON") : TEXT("OFF"),
		DebugCorruptEveryN > 0 ? *FString::Printf(TEXT(" (debug corruption every %d)"), DebugCorruptEveryN) : TEXT("")));

	FString TargetText = STS::EnumToString(TargetMode);
	if (TargetMode == ESTSTargetMode::RankRange)
	{
		TargetText += FString::Printf(TEXT(" [%d..%d]"), RankMin, RankMax);
	}
	else if (TargetMode == ESTSTargetMode::Patch)
	{
		TargetText += FString::Printf(TEXT(" [centre %d,%d size %dx%d step %d,%d]"), PatchCenterX, PatchCenterY, PatchSizeX, PatchSizeY, PatchStepX, PatchStepY);
	}
	OutLines.Add(FString::Printf(TEXT("  Updates (at stop): pattern %s, target %s, seed %d%s, impossible-step policy %s, retarget in flight %s"),
		*STS::EnumToString(UpdatePattern), *TargetText, UpdateSeed, bResetSeedPerTrigger ? TEXT(" (reset per trigger)") : TEXT(""),
		*STS::EnumToString(ImpossibleStepPolicy), bAllowRetargetInFlight ? TEXT("yes") : TEXT("no")));

	OutLines.Add(FString::Printf(TEXT("  Requests issued %lld | responses released %lld (%.2f MB) | stale discarded %lld | superseded %lld | late %lld | checksum failures %lld"),
		Window.Issued, Window.Released, Window.ReleasedBytes / STS::MegaByte, Window.Stale, Window.Superseded, Window.Late, Window.ChecksumFailures));
	OutLines.Add(StatsLine(TEXT("Issue -> release latency (ms):"), Window.LatencyMs));
	OutLines.Add(StatsLine(TEXT("Modelled latency (ms):"), Window.ModelLatencyMs));
	OutLines.Add(StatsLine(TEXT("Release lag behind model (ms):"), Window.ReleaseLagMs));

	if (Window.BurstCount > 0)
	{
		OutLines.Add(FString::Printf(TEXT("  Bursts: %d, requested %lld, issued %lld (shortfall + skipped %lld)"), Window.BurstCount, Window.BurstRequested, Window.BurstIssued, Window.BurstRequested - Window.BurstIssued));
		OutLines.Append(Window.BurstLines);
		if (Window.BurstCount > Window.BurstLines.Num())
		{
			OutLines.Add(FString::Printf(TEXT("  ... %d more bursts not listed"), Window.BurstCount - Window.BurstLines.Num()));
		}
	}
	if (Window.SpreadRequested > 0)
	{
		OutLines.Add(FString::Printf(TEXT("  Spread: requested %lld, issued %lld (shortfall + skipped %lld)"), Window.SpreadRequested, Window.SpreadIssued, Window.SpreadRequested - Window.SpreadIssued));
	}
	OutLines.Add(FString::Printf(TEXT("  Selection hash chain over every trigger in this recording: %016llX"), Window.SelectionHashChain));

	OutLines.Add(TEXT("  Harness cost on the game thread, per frame (ms):"));
	OutLines.Add(StatsLine(TEXT("ConsumerMs:"), Window.ConsumerMs));
	OutLines.Add(StatsLine(TEXT("ChecksumMs (part of Consumer):"), Window.ChecksumMs));
	OutLines.Add(StatsLine(TEXT("IssueMs:"), Window.IssueMs));
	if (Window.ChecksumBytes > 0)
	{
		OutLines.Add(FString::Printf(TEXT("  Checksum cost: %.1f ms total over %.1f MB = %.3f ms/MB (worst single response %.2f ms)"),
			Window.ChecksumMsTotal, Window.ChecksumBytes / STS::MegaByte, Window.ChecksumMsTotal / (Window.ChecksumBytes / STS::MegaByte), Window.ChecksumMsWorst));
	}
}

void ASTSTestbench::AppendUpdateHud(TArray<TPair<FString, FLinearColor>>& Lines, double NowSec) const
{
	if (!Server && !bHasLastBurst && !bScenarioRunning)
	{
		return;
	}

	Lines.Emplace(FString::Printf(TEXT("Server: in flight %d | issued %.1f/s | released %.1f/s | %.1f MB/s | latency %.1f (max %.1f) ms | consumer %.3f (max %.3f) ms"),
		Server ? Server->GetInFlight() : 0, HudStats.IssuedPerSec, HudStats.ReleasedPerSec, HudStats.MBPerSec, HudStats.LatencyAvgMs, HudStats.ShownLatencyMaxMs, HudStats.ConsumerAvgMs, HudStats.ShownConsumerMaxMs), FLinearColor::White);

	FString Checksum = bVerifyChecksums ? TEXT("checksums ON") : TEXT("checksums OFF");
	if (TotalChecksumBytes > 0.0)
	{
		Checksum += FString::Printf(TEXT(" (%.3f ms/MB, %.0f ms total)"), TotalChecksumMs / (TotalChecksumBytes / STS::MegaByte), TotalChecksumMs);
	}
	Lines.Emplace(FString::Printf(TEXT("%s | stale discarded %lld | total issued %lld | epoch %u"), *Checksum, TotalStaleDiscarded, TotalRequestsIssued, TileEpoch), FLinearColor::White);
	if (TotalChecksumFailures > 0)
	{
		Lines.Emplace(FString::Printf(TEXT("CHECKSUM FAILURES: %lld"), TotalChecksumFailures), FLinearColor::Red);
	}

	FString Logical = TEXT("Logical / visual tiles");
	for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
	{
		Logical += FString::Printf(TEXT(" | %d: %d / %d"), STS::BucketResolutions[Bucket], LogicalCountsPerBucket[Bucket], TileCountsPerBucket[Bucket]);
	}
	Lines.Emplace(Logical, FLinearColor::White);

	FString Triggers = FString::Printf(TEXT("Pattern %s | target %s | seed %d | "), *STS::EnumToString(UpdatePattern), *STS::EnumToString(TargetMode), UpdateSeed);
	Triggers += Spread.bActive ? FString::Printf(TEXT("spread ON %.1f/s (%lld due, %.1f s)"), Spread.Rate, Spread.NextIndex, NowSec - Spread.StartSec) : FString(TEXT("spread off"));
	if (BurstRepeat.bActive)
	{
		Triggers += FString::Printf(TEXT(" | burst repeat %.1f/s (%lld)"), BurstRepeat.Rate, BurstRepeat.NextIndex);
	}
	Lines.Emplace(Triggers, Spread.bActive || BurstRepeat.bActive ? FLinearColor::Yellow : FLinearColor::White);

	if (bHasLastBurst)
	{
		Lines.Emplace(FString::Printf(TEXT("Last burst: requested %d, issued %d (shortfall %d, skipped %d) | 4k %d/%d, 2k %d/%d, 1k %d/%d, 512 %d/%d issued/requested | hash %08X"),
			LastBurst.Requested, LastBurst.Issued, LastBurst.Shortfall, LastBurst.Skipped,
			LastBurst.IssuedByBucket[3], LastBurst.RequestedByBucket[3], LastBurst.IssuedByBucket[2], LastBurst.RequestedByBucket[2],
			LastBurst.IssuedByBucket[1], LastBurst.RequestedByBucket[1], LastBurst.IssuedByBucket[0], LastBurst.RequestedByBucket[0],
			uint32(LastBurst.SelectionHash)), LastBurst.Issued < LastBurst.Requested ? FLinearColor::Yellow : FLinearColor::White);
	}

	if (bScenarioRunning && ScenarioRuntimeSteps.IsValidIndex(ScenarioStepIndex))
	{
		Lines.Emplace(FString::Printf(TEXT("Scenario: step %d/%d %s (%.1f s)"), ScenarioStepIndex + 1, ScenarioRuntimeSteps.Num(),
			*STS::EnumToString(ScenarioRuntimeSteps[ScenarioStepIndex].Type), NowSec - ScenarioStepStartSec), FLinearColor::Yellow);
	}
}

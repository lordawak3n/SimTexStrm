// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSTestbench.h"

#include "SimTextureStrm.h"
#include "STSDxt1Generator.h"
#include "Engine/Texture2D.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "TextureResource.h"

namespace
{
	constexpr int32 MinAutoPoolSlack = 32;
	constexpr int32 MaxLoggedPoolEvents = 8;
	constexpr int32 MaxLoggedMismatches = 8;
	constexpr int32 MaxBurstBookmarks = 256;

	const TCHAR* const BucketLabels[STS::NumBuckets] = { TEXT("512"), TEXT("1k"), TEXT("2k"), TEXT("4k") };

	double ElapsedMsSince(double StartSec)
	{
		return (FPlatformTime::Seconds() - StartSec) * 1000.0;
	}

	FString StatsLine(const TCHAR* Label, const TArray<float>& Values)
	{
		const FSTSColumnStats Stats = FSTSRecorder::ComputeStats(Values);
		return FString::Printf(TEXT("  %-30s avg %.3f | p50 %.3f | p99 %.3f | max %.3f (n=%d)"), Label, Stats.Average, Stats.P50, Stats.P99, Stats.Max, Stats.Count);
	}

	/** Nearest-rank value of an ascending array. */
	float NearestRank(const TArray<float>& Sorted, double Fraction)
	{
		const int32 Index = FMath::Clamp(FMath::CeilToInt(Fraction * Sorted.Num()) - 1, 0, Sorted.Num() - 1);
		return Sorted[Index];
	}

	float MaxOf(const TArray<float>& Values)
	{
		return FSTSRecorder::ComputeStats(Values).Max;
	}
}

// --- Pools and builder ---

int32 ASTSTestbench::GetPoolSlack(int32 Bucket, int32 TilesAtBucket) const
{
	int32 Knob = -1;
	if (CommandLineSlack >= 0)
	{
		Knob = CommandLineSlack;
	}
	else
	{
		switch (Bucket)
		{
		case 0: Knob = PoolSlack512; break;
		case 1: Knob = PoolSlack1k; break;
		case 2: Knob = PoolSlack2k; break;
		default: Knob = PoolSlack4k; break;
		}
	}
	return Knob >= 0 ? Knob : FMath::Max(MinAutoPoolSlack, TilesAtBucket);
}

void ASTSTestbench::CreatePoolSpares(const int32* TilesPerBucket)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(STS_CreatePoolSpares);
	const double StartSec = FPlatformTime::Seconds();

	// A Regions spare is a complete slot (mips and GPU resource, created once); a Baseline spare is an empty object that gets both on first use.
	const bool bRegions = ActiveUploadMode == ESTSUploadMode::Regions;
	if (bRegions)
	{
		TSet<int32> Needed;
		for (const int32 Resolution : STS::BucketResolutions)
		{
			Needed.Add(Resolution);
		}
		EnsurePayloadLibrary(Needed);
	}

	for (int32 Bucket = 0; Bucket < STS::NumBuckets; ++Bucket)
	{
		PoolSlackUsed[Bucket] = GetPoolSlack(Bucket, TilesPerBucket[Bucket]);
		for (int32 Index = 0; Index < PoolSlackUsed[Bucket]; ++Index)
		{
			UTexture2D* Spare = bRegions ? CreateTileTexture(STS::BucketResolutions[Bucket]) : FSTSTexturePool::CreateSpareTexture(STS::BucketResolutions[Bucket], bSRGB);
			if (!Spare)
			{
				UE_LOG(LogSTS, Error, TEXT("Could not create spare texture %d of %d at %d."), Index, PoolSlackUsed[Bucket], STS::BucketResolutions[Bucket]);
				break;
			}
			TexturePool.AddSpare(Spare, Bucket);
		}
	}

	UE_LOG(LogSTS, Log, TEXT("Texture pools (%s): spares 4k %d, 2k %d, 1k %d, 512 %d (for %d/%d/%d/%d tiles) created in %.1f ms."),
		bRegions ? TEXT("Regions slots with GPU resources") : TEXT("empty objects"),
		PoolSlackUsed[3], PoolSlackUsed[2], PoolSlackUsed[1], PoolSlackUsed[0],
		TilesPerBucket[3], TilesPerBucket[2], TilesPerBucket[1], TilesPerBucket[0], ElapsedMsSince(StartSec));
}

void ASTSTestbench::RebuildPoolsFromTiles()
{
	if (Tiles.IsEmpty())
	{
		return;
	}

	TexturePool.Reset();
	for (const FSTSTile& Tile : Tiles)
	{
		const int32 Bucket = STS::ResolutionToBucket(Tile.Resolution);
		if (Bucket != INDEX_NONE && Tile.Texture)
		{
			TexturePool.AddInUse(Tile.Texture, Bucket);
		}
	}
	CreatePoolSpares(TileCountsPerBucket);
}

void ASTSTestbench::EnsureBuilder()
{
	const int32 Threads = FMath::Clamp(BaselineBuilderThreads, 1, 16);
	if (Builder && Builder->GetThreadCount() != Threads)
	{
		ShutdownBuilder();
	}
	if (!Builder)
	{
		Builder = MakeUnique<FSTSMipBuilder>(Threads);
		Builder->SetEpoch(TileEpoch);
		UE_LOG(LogSTS, Log, TEXT("Mip builder started with %d thread(s)."), Threads);
	}
}

void ASTSTestbench::ShutdownBuilder()
{
	Builder.Reset();
	ApplyQueue.Empty();
	FinishedScratch.Empty();
	BurstBookmarks.Empty();
}

bool ASTSTestbench::IsUploadPathIdle() const
{
	if (STS::IsBaselineFamily(ActiveUploadMode))
	{
		return (!Builder || Builder->GetOutstanding() == 0) && ApplyQueue.IsEmpty();
	}
	if (ActiveUploadMode == ESTSUploadMode::Regions)
	{
		return UploadQueue.IsEmpty() && InFlightUploads.IsEmpty();
	}
	return true;
}

void ASTSTestbench::RefreshPoolStats()
{
	if (STS::UpdatesVisuals(ActiveUploadMode))
	{
		TexturePool.ComputeRetainedBytes(RetainedBulkBytes);
	}
	for (int32 Bucket = 0; Bucket < STS::NumBuckets; ++Bucket)
	{
		PoolVramMB[Bucket] = ActiveUploadMode == ESTSUploadMode::Regions
			? float(TexturePool.GetNumMembers(Bucket) * FSTSDxt1Generator::GetChainBytes(STS::BucketResolutions[Bucket], STS::BucketResolutions[Bucket]) / STS::MegaByte)
			: 0.0f;
	}
}

// --- Consumer: build and apply ---

void ASTSTestbench::SubmitBuildJob(FSTSResponse& Response, double NowSec)
{
	if (!Builder)
	{
		UE_LOG(LogSTS, Error, TEXT("Response for request %lld dropped: the mip builder is not running."), Response.Request.RequestId);
		return;
	}

	FSTSBuildJob Job;
	Job.Request = Response.Request;
	Job.Source = Response.Source;
	Job.Buffer = MoveTemp(Response.Buffer);
	Job.ReleaseTimeSec = NowSec;
	Job.bSingleUse = ActiveUploadMode == ESTSUploadMode::BaselineSingleUse;
	Builder->Submit(MoveTemp(Job));
}

void ASTSTestbench::NoteBurstIssued(int64 FirstRequestId, int64 EndRequestId)
{
	++BurstSerial;
	if (EndRequestId <= FirstRequestId)
	{
		return;
	}
	if (BurstBookmarks.Num() >= MaxBurstBookmarks)
	{
		BurstBookmarks.RemoveAt(0);
	}
	FBurstBookmark& Bookmark = BurstBookmarks.AddDefaulted_GetRef();
	Bookmark.FirstRequestId = FirstRequestId;
	Bookmark.EndRequestId = EndRequestId;
	Bookmark.Number = BurstSerial;
}

void ASTSTestbench::ApplyBaselineJobs()
{
	if (!Builder)
	{
		return;
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(STS_ApplyBaseline);

	// Render-thread samples written since last frame; the markers run about one frame after the game thread enqueues them.
	RenderSamplesScratch.Reset();
	RenderTimer.Drain(RenderSamplesScratch);
	for (const FSTSRenderTimingSample& Sample : RenderSamplesScratch)
	{
		Frame.RTCreateMs += Sample.Ms;
		if (Recorder.IsRecording() && Sample.Bucket >= 0 && Sample.Bucket < STS::NumBuckets)
		{
			Window.RTCreateMs[Sample.Bucket].Add(Sample.Ms);
		}
	}

	FinishedScratch.Reset();
	Builder->Drain(FinishedScratch);
	for (FSTSBuildJob& Job : FinishedScratch)
	{
		if (!Job.bBuildSkipped)
		{
			Frame.BuildMs += Job.BuildMs;
			const int32 Bucket = STS::ResolutionToBucket(Job.Request.Resolution);
			if (Recorder.IsRecording() && Bucket != INDEX_NONE)
			{
				Window.BuildMs[Bucket].Add(float(Job.BuildMs));
			}
		}
		ApplyQueue.Add(MoveTemp(Job));
	}
	FinishedScratch.Reset();

	if (ApplyQueue.IsEmpty())
	{
		return;
	}

	const double StartSec = FPlatformTime::Seconds();
	int32 Kept = 0;
	for (int32 Index = 0; Index < ApplyQueue.Num(); ++Index)
	{
		if (!ApplyBuildJob(ApplyQueue[Index]))
		{
			if (Kept != Index)
			{
				ApplyQueue[Kept] = MoveTemp(ApplyQueue[Index]);
			}
			++Kept;
		}
	}
	if (Kept < ApplyQueue.Num())
	{
		ApplyQueue.RemoveAt(Kept, ApplyQueue.Num() - Kept);
	}
	Frame.ApplyMs += ElapsedMsSince(StartSec);
}

bool ASTSTestbench::ApplyBuildJob(FSTSBuildJob& Job)
{
	const FSTSRequest& Request = Job.Request;
	const bool bRecording = Recorder.IsRecording();

	if (Request.TileEpoch != TileEpoch || Job.bBuildSkipped)
	{
		++Frame.Stale;
		++TotalStaleDiscarded;
		if (bRecording)
		{
			++Window.StaleBuildJobs;
		}
		return true;
	}

	if (!Tiles.IsValidIndex(Request.TileIndex))
	{
		return true;
	}
	FSTSTile& Tile = Tiles[Request.TileIndex];
	if (Tile.PendingRequestId != Request.RequestId)
	{
		if (bRecording)
		{
			++Window.Superseded;
		}
		return true;
	}

	const int32 NewBucket = STS::ResolutionToBucket(Request.Resolution);
	const int32 OldBucket = STS::ResolutionToBucket(Tile.Resolution);
	if (NewBucket == INDEX_NONE || OldBucket == INDEX_NONE || Job.Mips.IsEmpty() || !Job.Source.IsValid())
	{
		UE_LOG(LogSTS, Error, TEXT("Build job for request %lld is unusable (resolution %d, %d mips); dropped."), Request.RequestId, Request.Resolution, Job.Mips.Num());
		Tile.PendingRequestId = INDEX_NONE;
		return true;
	}

	UTexture2D* Texture = TakePooledTexture(NewBucket, Request, Job.bExhaustedCounted);
	if (!Texture)
	{
		return false;
	}

	FTexturePlatformData* PlatformData = Texture->GetPlatformData();
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(STS_PlatformDataSwap);
		PlatformData->Mips.Empty();
		for (TUniquePtr<FTexture2DMipMap>& Mip : Job.Mips)
		{
			PlatformData->Mips.Add(Mip.Release());
		}
		Job.Mips.Reset();
	}

	RenderTimer.EnqueueBegin();
	const double UpdateStartSec = FPlatformTime::Seconds();
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(STS_UpdateResource);
		Texture->UpdateResource();
	}
	const double UpdateMs = ElapsedMsSince(UpdateStartSec);
	RenderTimer.EnqueueEnd(NewBucket);
	Frame.UpdateResourceMs += UpdateMs;

	CommitTileUpdate(Tile, Texture, Request, OldBucket, NewBucket, Job.ReleaseTimeSec, Job.Source->Data.Num());
	return true;
}

UTexture2D* ASTSTestbench::TakePooledTexture(int32 Bucket, const FSTSRequest& Request, bool& bInOutExhaustedCounted)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(STS_SlotAcquire);

	uint64 ReleaseFrame = FSTSTexturePool::NeverReleased;
	UTexture2D* Texture = TexturePool.TakeBottom(Bucket, ReleaseFrame);
	if (!Texture)
	{
		if (!bInOutExhaustedCounted)
		{
			bInOutExhaustedCounted = true;
			++Frame.PoolExhausted;
			++TotalPoolExhausted;
			if (LoggedPoolExhausted < MaxLoggedPoolEvents)
			{
				++LoggedPoolExhausted;
				UE_LOG(LogSTS, Warning, TEXT("Pool exhausted: the %d queue is empty for request %lld (tile %d); the update waits for the next frame."),
					Request.Resolution, Request.RequestId, Request.TileIndex);
			}
		}
		return nullptr;
	}

	if (ReleaseFrame != FSTSTexturePool::NeverReleased)
	{
		const int32 Age = int32(FMath::Min<uint64>(GFrameCounter - ReleaseFrame, MAX_int32));
		Frame.MinReuseAge = Frame.MinReuseAge < 0 ? Age : FMath::Min(Frame.MinReuseAge, Age);
		if (Recorder.IsRecording())
		{
			Window.ReuseAgeFrames.Add(float(Age));
		}
		if (Age < SafeReuseFrames)
		{
			++Frame.UnsafeReuses;
			++TotalUnsafeReuses;
			if (LoggedUnsafeReuses < MaxLoggedPoolEvents)
			{
				++LoggedUnsafeReuses;
				UE_LOG(LogSTS, Warning, TEXT("Unsafe reuse: a %d texture released %d frame(s) ago was taken again (SafeReuseFrames %d), request %lld."),
					Request.Resolution, Age, SafeReuseFrames, Request.RequestId);
			}
		}
	}
	return Texture;
}

void ASTSTestbench::CommitTileUpdate(FSTSTile& Tile, UTexture2D* Texture, const FSTSRequest& Request, int32 OldBucket, int32 NewBucket, double ReleaseTimeSec, int64 PayloadBytes)
{
	UTexture2D* OldTexture = Tile.Texture;
	Tile.Material->SetTextureParameterValue(TextureParameterName, Texture);
	const double VisibleSec = FPlatformTime::Seconds();
	Tile.Texture = Texture;
	if (OldTexture)
	{
		TexturePool.Release(OldTexture, OldBucket, GFrameCounter);
	}

	--TileCountsPerBucket[OldBucket];
	++TileCountsPerBucket[NewBucket];
	Tile.Resolution = Request.Resolution;
	Tile.VisualVariant = Request.Variant;
	Tile.PendingRequestId = INDEX_NONE;
	++Tile.UpdateCount;

	++Frame.Uploads;
	++TotalUploads;
	Frame.UploadBytes += PayloadBytes;
	const double IssueToVisibleMs = (VisibleSec - Request.IssueTimeSec) * 1000.0;
	Frame.IssueToVisibleSumMs += IssueToVisibleMs;
	Frame.IssueToVisibleMaxMs = FMath::Max(Frame.IssueToVisibleMaxMs, IssueToVisibleMs);
	Frame.ReleaseToVisibleMaxMs = FMath::Max(Frame.ReleaseToVisibleMaxMs, (VisibleSec - ReleaseTimeSec) * 1000.0);
	if (Recorder.IsRecording())
	{
		Window.IssueToVisibleMs.Add(float(IssueToVisibleMs));
	}

	for (int32 Index = 0; Index < BurstBookmarks.Num(); ++Index)
	{
		const FBurstBookmark& Bookmark = BurstBookmarks[Index];
		if (Request.RequestId >= Bookmark.FirstRequestId && Request.RequestId < Bookmark.EndRequestId)
		{
			TRACE_BOOKMARK(TEXT("STS Burst %d: first %s (request %lld)"), Bookmark.Number, ActiveUploadMode == ESTSUploadMode::Regions ? TEXT("upload") : TEXT("apply"), Request.RequestId);
			BurstBookmarks.RemoveAtSwap(Index);
			break;
		}
	}
}

void ASTSTestbench::FillBaselineColumns(FSTSFrameSample& Sample)
{
	Sample.Uploads = Frame.Uploads;
	Sample.UploadMB = float(Frame.UploadBytes / STS::MegaByte);
	Sample.BuildMs = float(Frame.BuildMs);
	Sample.BuildQueueDepth = Builder ? Builder->GetOutstanding() : 0;
	Sample.ApplyGTMs = float(Frame.ApplyMs);
	Sample.UpdateResourceGTMs = float(Frame.UpdateResourceMs);
	Sample.RTCreateMs = float(Frame.RTCreateMs);
	for (int32 Bucket = 0; Bucket < STS::NumBuckets; ++Bucket)
	{
		Sample.FreeObjects[Bucket] = TexturePool.GetNumFree(Bucket);
	}
	Sample.MinReuseAgeFrames = Frame.MinReuseAge;
	Sample.UnsafeReuses = Frame.UnsafeReuses;
	Sample.PoolExhausted = Frame.PoolExhausted;
	Sample.IssueToVisibleAvgMs = Frame.Uploads > 0 ? float(Frame.IssueToVisibleSumMs / Frame.Uploads) : 0.0f;
	Sample.IssueToVisibleMaxMs = float(Frame.IssueToVisibleMaxMs);
	Sample.ReleaseToVisibleMaxMs = float(Frame.ReleaseToVisibleMaxMs);
	FillRegionsColumns(Sample);

	if (!Recorder.IsRecording())
	{
		return;
	}

	Window.Uploads += Frame.Uploads;
	Window.UploadBytes += Frame.UploadBytes;
	Window.UnsafeReuses += Frame.UnsafeReuses;
	Window.PoolExhausted += Frame.PoolExhausted;
	if (Frame.Uploads > 0)
	{
		if (STS::IsBaselineFamily(ActiveUploadMode))
		{
			Window.ApplyGTMs.Add(Sample.ApplyGTMs);
			Window.UpdateResourceGTMs.Add(Sample.UpdateResourceGTMs);
			Window.ApplyStepGTMs.Add(Sample.ApplyGTMs);
		}
		else
		{
			Window.ApplyStepGTMs.Add(float(Frame.RegionsApplyMs));
		}
	}

	Window.PeakProcessPhysMB = FMath::Max(Window.PeakProcessPhysMB, Sample.ProcessPhysMB);
	Window.PeakRHITextureMB = FMath::Max(Window.PeakRHITextureMB, Sample.RHITextureMB);
	Window.PeakGpuLocalMemMB = FMath::Max(Window.PeakGpuLocalMemMB, Sample.GpuLocalMemMB);
	Window.EndProcessPhysMB = Sample.ProcessPhysMB;
	Window.EndRHITextureMB = Sample.RHITextureMB;
	Window.EndGpuLocalMemMB = Sample.GpuLocalMemMB;
}

// --- Consistency ---

void ASTSTestbench::CheckConsistency()
{
	if (!IsGameWorldValid(TEXT("CheckConsistency")))
	{
		return;
	}
	RunConsistencyCheck();
}

int32 ASTSTestbench::RunConsistencyCheck()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(STS_CheckConsistency);

	// In LogicalOnly mode visuals never follow the logical state, so only the texture-level checks apply.
	const bool bCompareLogical = STS::UpdatesVisuals(ActiveUploadMode);
	int32 Mismatches = 0;
	int32 TilesWithUpdateInFlight = 0;
	TArray<FString> FirstMismatches;

	const auto Report = [&](int32 TileIndex, const FString& Text)
	{
		++Mismatches;
		if (FirstMismatches.Num() < MaxLoggedMismatches)
		{
			const FSTSTile& Tile = Tiles[TileIndex];
			FirstMismatches.Add(FString::Printf(TEXT("tile %d (cell %d,%d, rank %d): %s"), TileIndex, Tile.CellX, Tile.CellY, Tile.Rank, *Text));
		}
	};

	for (int32 TileIndex = 0; TileIndex < Tiles.Num(); ++TileIndex)
	{
		const FSTSTile& Tile = Tiles[TileIndex];
		if (Tile.PendingRequestId != INDEX_NONE)
		{
			++TilesWithUpdateInFlight;
		}

		if (bCompareLogical)
		{
			if (Tile.Resolution != Tile.LogicalResolution)
			{
				Report(TileIndex, FString::Printf(TEXT("visual resolution %d, logical %d"), Tile.Resolution, Tile.LogicalResolution));
			}
			if (Tile.VisualVariant != Tile.LogicalVariant)
			{
				Report(TileIndex, FString::Printf(TEXT("visual variant %d, logical %d"), Tile.VisualVariant, Tile.LogicalVariant));
			}
		}

		const UTexture2D* Texture = Tile.Texture;
		if (!Texture)
		{
			Report(TileIndex, TEXT("no texture"));
			continue;
		}

		const FTexturePlatformData* PlatformData = Texture->GetPlatformData();
		const int32 SizeX = PlatformData ? PlatformData->SizeX : 0;
		const int32 SizeY = PlatformData ? PlatformData->SizeY : 0;
		if (SizeX != Tile.Resolution || SizeY != Tile.Resolution)
		{
			Report(TileIndex, FString::Printf(TEXT("texture is %dx%d, visual resolution %d"), SizeX, SizeY, Tile.Resolution));
		}
		else if (const FTextureResource* Resource = Texture->GetResource())
		{
			if (int32(Resource->GetSizeX()) != Tile.Resolution || int32(Resource->GetSizeY()) != Tile.Resolution)
			{
				Report(TileIndex, FString::Printf(TEXT("texture resource is %dx%d, visual resolution %d"), Resource->GetSizeX(), Resource->GetSizeY(), Tile.Resolution));
			}
		}

		const UTexture* MaterialTexture = Tile.Material ? Tile.Material->K2_GetTextureParameterValue(TextureParameterName) : nullptr;
		if (MaterialTexture != Texture)
		{
			Report(TileIndex, FString::Printf(TEXT("MID texture is '%s', tile texture is '%s'"),
				MaterialTexture ? *MaterialTexture->GetName() : TEXT("none"), *Texture->GetName()));
		}

		if (ActiveUploadMode == ESTSUploadMode::Regions)
		{
			const FRegionsSlot* Slot = RegionsSlots.Find(Texture);
			if (!Slot || !Slot->bInitialized)
			{
				Report(TileIndex, TEXT("slot was never initialised"));
			}
			else if (Slot->Resource != Texture->GetResource())
			{
				Report(TileIndex, TEXT("slot's GPU resource was replaced after initialisation"));
			}
		}
	}

	TotalConsistencyMismatches += Mismatches;
	if (Recorder.IsRecording())
	{
		++Window.ConsistencyChecks;
		Window.ConsistencyMismatches += Mismatches;
	}

	UE_LOG(LogSTS, Log, TEXT("Consistency check (%s): %d tiles, %d mismatches%s."), *STS::EnumToString(ActiveUploadMode), Tiles.Num(), Mismatches,
		TilesWithUpdateInFlight > 0 ? *FString::Printf(TEXT("; %d tiles still have an update in flight"), TilesWithUpdateInFlight) : TEXT(""));
	for (const FString& Text : FirstMismatches)
	{
		UE_LOG(LogSTS, Error, TEXT("  Mismatch: %s"), *Text);
	}
	if (Mismatches > FirstMismatches.Num())
	{
		UE_LOG(LogSTS, Error, TEXT("  ... %d more mismatches not listed"), Mismatches - FirstMismatches.Num());
	}
	return Mismatches;
}

// --- Summary and HUD ---

void ASTSTestbench::GatherBaselineSummary(TArray<FString>& OutLines) const
{
	if (STS::IsBaselineFamily(ActiveUploadMode) && (!Tiles.IsEmpty() || Window.Uploads > 0))
	{
		OutLines.Add(FString());
		OutLines.Add(FString::Printf(TEXT("%s upload path (FIFO texture pools, background mip build, PlatformData swap, UpdateResource%s):"), *STS::EnumToString(ActiveUploadMode),
			ActiveUploadMode == ESTSUploadMode::BaselineSingleUse ? TEXT(", BULKDATA_SingleUse mips") : TEXT("")));
		OutLines.Add(FString::Printf(TEXT("  Mip builder: %d thread(s); SafeReuseFrames %d"), Builder ? Builder->GetThreadCount() : BaselineBuilderThreads, SafeReuseFrames));

		int64 Retained[STS::NumBuckets];
		TexturePool.ComputeRetainedBytes(Retained);
		int64 RetainedTotal = 0;
		for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
		{
			RetainedTotal += Retained[Bucket];
			OutLines.Add(FString::Printf(TEXT("  Pool %-4s: slack %d spares created | in use %d | free %d at stop | retained PlatformData %.1f MB"),
				BucketLabels[Bucket], PoolSlackUsed[Bucket], TexturePool.GetNumInUse(Bucket), TexturePool.GetNumFree(Bucket), Retained[Bucket] / STS::MegaByte));
		}
		OutLines.Add(FString::Printf(TEXT("  Retained PlatformData CPU memory, all pools: %.1f MB (sum of the mips whose bulk data is still loaded)"), RetainedTotal / STS::MegaByte));
		if (ActiveUploadMode == ESTSUploadMode::BaselineSingleUse)
		{
			OutLines.Add(TEXT("  SingleUse: UpdateResource's FTexture2DResource takes each mip's memory (FBulkData::GetCopy discards single-use data), so PlatformData keeps the mip sizes but no bytes."));
			OutLines.Add(TEXT("             Anything that reads those mips later (GetCopy, Lock, or another UpdateResource without replacing them) finds the bulk data unloaded and has no file to reload it from; this path never does,"));
			OutLines.Add(TEXT("             because every reuse replaces all mips before UpdateResource, and the harness only reads the PlatformData sizes."));
		}

		OutLines.Add(FString::Printf(TEXT("  Uploads applied %lld (%.1f MB) | unsafe reuses %lld | pool exhausted %lld | stale build jobs dropped %lld"),
			Window.Uploads, Window.UploadBytes / STS::MegaByte, Window.UnsafeReuses, Window.PoolExhausted, Window.StaleBuildJobs));

		if (Window.ReuseAgeFrames.IsEmpty())
		{
			OutLines.Add(TEXT("  Reuse age (frames since release): no reuse of a released texture was recorded"));
		}
		else
		{
			TArray<float> Sorted = Window.ReuseAgeFrames;
			Sorted.Sort();
			OutLines.Add(FString::Printf(TEXT("  Reuse age (frames since release): min %.0f | p1 %.0f | p50 %.0f (n=%d)"),
				Sorted[0], NearestRank(Sorted, 0.01), NearestRank(Sorted, 0.50), Sorted.Num()));
		}

		OutLines.Add(StatsLine(TEXT("Issue -> visible (ms):"), Window.IssueToVisibleMs));
		OutLines.Add(TEXT("  Per frame with uploads (ms):"));
		OutLines.Add(StatsLine(TEXT("ApplyGTMs:"), Window.ApplyGTMs));
		OutLines.Add(StatsLine(TEXT("UpdateResourceGTMs:"), Window.UpdateResourceGTMs));
		OutLines.Add(TEXT("  Per texture by resolution (ms):"));
		for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
		{
			OutLines.Add(StatsLine(*FString::Printf(TEXT("RTCreateMs %s:"), BucketLabels[Bucket]), Window.RTCreateMs[Bucket]));
		}
		for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
		{
			OutLines.Add(StatsLine(*FString::Printf(TEXT("Builder time %s:"), BucketLabels[Bucket]), Window.BuildMs[Bucket]));
		}

		OutLines.Add(TEXT("  Baseline timing sources:"));
		OutLines.Add(TEXT("    BuildMs: FPlatformTime on the builder thread per job (lock/realloc/memcpy/unlock of every mip), summed over the jobs drained this frame."));
		OutLines.Add(TEXT("    ApplyGTMs, UpdateResourceGTMs: FPlatformTime on the game thread (the whole apply step; only the UpdateResource call)."));
		OutLines.Add(TEXT("    RTCreateMs: render-thread FPlatformTime::Cycles64 delta between two render commands around UpdateResource; read back on the game thread, about 1 frame late."));
		OutLines.Add(TEXT("    Issue -> visible: the frame the MID was set (SetTextureParameterValue); the first draw with the texture is about one frame later."));
		OutLines.Add(TEXT("    GpuLocalMemMB: DXGI QueryVideoMemoryInfo(LOCAL).CurrentUsage of the RHI's adapter; -1 when unavailable."));
	}

	if (Window.ConsistencyChecks > 0)
	{
		OutLines.Add(FString::Printf(TEXT("  Consistency: %lld check(s), %lld mismatch(es)"), Window.ConsistencyChecks, Window.ConsistencyMismatches));
	}
}

void ASTSTestbench::GatherKeyStats(TArray<FString>& OutLines) const
{
	const FSTSColumnStats ConsumerStats = FSTSRecorder::ComputeStats(Window.ConsumerMs);
	const FSTSColumnStats VisibleStats = FSTSRecorder::ComputeStats(Window.IssueToVisibleMs);

	OutLines.Add(FString());
	OutLines.Add(TEXT("Key stats (one per line, read by RunBenchmark -Suite; -1 means not available):"));
	const auto AddInt = [&OutLines](const TCHAR* Name, int64 Value)
	{
		OutLines.Add(FString::Printf(TEXT("[Key] %s = %lld"), Name, Value));
	};
	const auto AddFloat = [&OutLines](const FString& Name, double Value)
	{
		OutLines.Add(FString::Printf(TEXT("[Key] %s = %.4f"), *Name, Value));
	};

	FString ProfileSummary;
	TArray<FString> UnusedRenderLines;
	const bool bProfileApplied = GatherRenderSettings(UnusedRenderLines, ProfileSummary);

	OutLines.Add(FString::Printf(TEXT("[Key] UploadMode = %s"), *STS::EnumToString(ActiveUploadMode)));
	OutLines.Add(FString::Printf(TEXT("[Key] Checksums = %s"), bVerifyChecksums ? TEXT("On") : TEXT("Off")));
	OutLines.Add(FString::Printf(TEXT("[Key] RunContext = %s"), IsPieWorld() ? TEXT("PIE") : TEXT("Standalone")));
	OutLines.Add(FString::Printf(TEXT("[Key] RenderProfile = %s"), bProfileApplied ? TEXT("Applied") : TEXT("NotApplied")));
	AddInt(TEXT("Uploads"), Window.Uploads);
	AddInt(TEXT("UnsafeReuses"), Window.UnsafeReuses);
	AddInt(TEXT("PoolExhausted"), Window.PoolExhausted);
	AddInt(TEXT("ChecksumFailures"), Window.ChecksumFailures);
	AddInt(TEXT("ConsistencyChecks"), Window.ConsistencyChecks);
	AddInt(TEXT("ConsistencyMismatches"), Window.ConsistencyMismatches);
	AddFloat(TEXT("IssueToVisibleP99Ms"), VisibleStats.P99);
	AddFloat(TEXT("IssueToVisibleMaxMs"), VisibleStats.Max);
	AddFloat(TEXT("ApplyGTMsMax"), MaxOf(Window.ApplyGTMs));
	AddFloat(TEXT("UpdateResourceGTMsMax"), MaxOf(Window.UpdateResourceGTMs));
	AddFloat(TEXT("ApplyStepGTMsMax"), MaxOf(Window.ApplyStepGTMs));
	AddFloat(TEXT("ConsumerMsAvg"), ConsumerStats.Average);
	AddFloat(TEXT("ConsumerMsMax"), ConsumerStats.Max);
	for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
	{
		const FSTSColumnStats RTStats = FSTSRecorder::ComputeStats(Window.RTCreateMs[Bucket]);
		AddFloat(FString::Printf(TEXT("RTCreateMsAvg_%s"), BucketLabels[Bucket]), RTStats.Average);
		AddFloat(FString::Printf(TEXT("RTCreateMsMax_%s"), BucketLabels[Bucket]), RTStats.Max);
	}

	// Phase 3: Regions mode and thread-time capture.
	AddInt(TEXT("UpdateResourceAfterInit"), Window.UpdateResourceAfterInit);
	AddInt(TEXT("BufferPoolMisses"), Window.BufferPoolMisses);
	AddInt(TEXT("UploadFailures"), Window.UploadFailures);
	AddInt(TEXT("ReadbackMipsChecked"), Window.ReadbackMipsChecked);
	AddInt(TEXT("ReadbackMipMismatches"), Window.ReadbackMipsMismatched);
	AddFloat(TEXT("UploadGTMsMax"), MaxOf(Window.UploadGTMs));
	for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
	{
		const FSTSColumnStats RTUploadStats = FSTSRecorder::ComputeStats(Window.RTUploadMs[Bucket]);
		AddFloat(FString::Printf(TEXT("RTUploadMsAvg_%s"), BucketLabels[Bucket]), RTUploadStats.Average);
		AddFloat(FString::Printf(TEXT("RTUploadMsMax_%s"), BucketLabels[Bucket]), RTUploadStats.Max);
		const FSTSColumnStats LatencyStats = FSTSRecorder::ComputeStats(Window.UploadLatencyMs[Bucket]);
		AddFloat(FString::Printf(TEXT("UploadLatencyP99Ms_%s"), BucketLabels[Bucket]), LatencyStats.P99);
		AddFloat(FString::Printf(TEXT("UploadLatencyMaxMs_%s"), BucketLabels[Bucket]), LatencyStats.Max);
	}
	float PoolVramTotalMB = 0.0f;
	for (const float MB : PoolVramMB)
	{
		PoolVramTotalMB += MB;
	}
	AddFloat(TEXT("PoolVramMB"), PoolVramTotalMB);
	int64 RetainedNow[STS::NumBuckets];
	TexturePool.ComputeRetainedBytes(RetainedNow);
	AddFloat(TEXT("RetainedPlatformDataMB"), double(RetainedNow[0] + RetainedNow[1] + RetainedNow[2] + RetainedNow[3]) / STS::MegaByte);
	if (Window.MipFit.N > 0)
	{
		double FixedCostUs = 0.0;
		double ThroughputGBps = 0.0;
		double R2 = 0.0;
		if (SolveCostFit(Window.MipFit, FixedCostUs, ThroughputGBps, R2))
		{
			AddFloat(TEXT("MipFixedCostUs"), FixedCostUs);
			AddFloat(TEXT("MipThroughputGBps"), ThroughputGBps);
			AddFloat(TEXT("MipFitR2"), R2);
			AddInt(TEXT("MipCallsPerFrameMax"), Window.MaxMipCallsPerFrame);
			AddFloat(TEXT("MipFixedCostPerBurstFrameMs"), FixedCostUs * Window.MaxMipCallsPerFrame / 1000.0);
		}
	}
	const double ThreadFrames = FMath::Max<int64>(1, Window.ThreadFrames);
	AddFloat(TEXT("SlatePresentsAvg"), Window.PresentsSum / ThreadFrames);
	AddInt(TEXT("SlatePresentsMax"), Window.PresentsMax);
	AddFloat(TEXT("RenderThreadMsRawAvg"), Window.RenderRawSum / ThreadFrames);
	AddFloat(TEXT("RHIThreadMsRawAvg"), Window.RHIRawSum / ThreadFrames);
	AddFloat(TEXT("ProcessPhysMBPeak"), Window.PeakProcessPhysMB);
	AddFloat(TEXT("ProcessPhysMBEnd"), Window.EndProcessPhysMB);
	AddFloat(TEXT("RHITextureMBPeak"), Window.PeakRHITextureMB);
	AddFloat(TEXT("RHITextureMBEnd"), Window.EndRHITextureMB);
	AddFloat(TEXT("GpuLocalMemMBPeak"), Window.PeakGpuLocalMemMB);
	AddFloat(TEXT("GpuLocalMemMBEnd"), Window.EndGpuLocalMemMB);
}

void ASTSTestbench::AppendBaselineHud(TArray<TPair<FString, FLinearColor>>& Lines) const
{
	if (!STS::IsBaselineFamily(ActiveUploadMode) || (Tiles.IsEmpty() && !bCreationInProgress && TotalUploads == 0))
	{
		return;
	}

	FString Pools = TEXT("Pools free / in use (retained PlatformData)");
	for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
	{
		Pools += FString::Printf(TEXT(" | %s: %d / %d (%.0f MB)"), BucketLabels[Bucket], TexturePool.GetNumFree(Bucket), TexturePool.GetNumInUse(Bucket), RetainedBulkBytes[Bucket] / STS::MegaByte);
	}
	Lines.Emplace(Pools, FLinearColor::White);

	const bool bProblem = TotalUnsafeReuses > 0 || TotalPoolExhausted > 0 || TotalConsistencyMismatches > 0;
	Lines.Emplace(FString::Printf(TEXT("Uploads %lld | builder %d, apply queue %d | unsafe reuses %lld | pool exhausted %lld | consistency mismatches %lld"),
		TotalUploads, Builder ? Builder->GetOutstanding() : 0, ApplyQueue.Num(), TotalUnsafeReuses, TotalPoolExhausted, TotalConsistencyMismatches),
		bProblem ? FLinearColor::Red : FLinearColor::White);
}

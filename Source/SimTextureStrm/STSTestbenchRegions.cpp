// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSTestbench.h"

#include "SimTextureStrm.h"
#include "Engine/Texture2D.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "TextureResource.h"

namespace
{
	constexpr int32 MaxLoggedUploadFailures = 8;
	constexpr int32 MaxReportedReadbackMismatches = 12;
	constexpr int64 DxtBlockBytes = 8;

	const TCHAR* const BucketLabels[STS::NumBuckets] = { TEXT("512"), TEXT("1k"), TEXT("2k"), TEXT("4k") };

	double ElapsedMsSince(double StartSec)
	{
		return (FPlatformTime::Seconds() - StartSec) * 1000.0;
	}

	FString StatsLine(const TCHAR* Label, const TArray<float>& Values)
	{
		const FSTSColumnStats Stats = FSTSRecorder::ComputeStats(Values);
		return FString::Printf(TEXT("  %-30s avg %.4f | p50 %.4f | p99 %.4f | max %.4f (n=%d)"), Label, Stats.Average, Stats.P50, Stats.P99, Stats.Max, Stats.Count);
	}

	float NearestRank(const TArray<float>& Sorted, double Fraction)
	{
		const int32 Index = FMath::Clamp(FMath::CeilToInt(Fraction * Sorted.Num()) - 1, 0, Sorted.Num() - 1);
		return Sorted[Index];
	}

	/** What a readback render command compares: one texture against the payload it should hold. */
	struct FReadbackJob
	{
		FString Label;
		FTextureResource* Resource = nullptr;
		TSharedPtr<const FSTSPayload> Expected;
	};

	struct FReadbackResult
	{
		int32 MipsChecked = 0;
		int32 MipsMismatched = 0;
		TArray<FString> Lines;
	};
}

// --- Setup and teardown ---

void ASTSTestbench::EnsureRegionsInfrastructure()
{
	int32 Sizes[STS::NumBuckets] = { PayloadBufferPool512, PayloadBufferPool1k, PayloadBufferPool2k, PayloadBufferPool4k };
	if (PayloadBufferPoolCommandLine >= 0)
	{
		for (int32& Size : Sizes)
		{
			Size = PayloadBufferPoolCommandLine;
		}
	}

	bool bSameSizes = BufferPool.IsValid();
	for (int32 Bucket = 0; Bucket < STS::NumBuckets; ++Bucket)
	{
		Sizes[Bucket] = FMath::Max(0, Sizes[Bucket]);
		bSameSizes = bSameSizes && Sizes[Bucket] == BufferPoolSizes[Bucket];
	}

	if (!bSameSizes)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(STS_CreateBufferPool);
		const double StartSec = FPlatformTime::Seconds();
		BufferPool = MakeShared<FSTSPayloadBufferPool, ESPMode::ThreadSafe>(Sizes);
		FMemory::Memcpy(BufferPoolSizes, Sizes);
		LastBufferPoolMisses = 0;
		UE_LOG(LogSTS, Log, TEXT("Payload buffer pool: %d x 4k, %d x 2k, %d x 1k, %d x 512 (%.1f MB reserved) in %.1f ms."),
			Sizes[3], Sizes[2], Sizes[1], Sizes[0], BufferPool->GetPooledBytes() / STS::MegaByte, ElapsedMsSince(StartSec));
	}

	if (!Uploader)
	{
		Uploader = MakeUnique<FSTSRegionsUploader>(UploadTimer, MipTimer);
	}
	if (Server)
	{
		Server->SetBufferPool(BufferPool);
	}
}

void ASTSTestbench::ShutdownRegions()
{
	if (Server)
	{
		Server->SetBufferPool(nullptr);
	}
	UploadQueue.Reset();
	InFlightUploads.Reset();
	RegionsSlots.Reset();
	Uploader.Reset();
	BufferPool.Reset();
	FMemory::Memzero(BufferPoolSizes);
	LastBufferPoolMisses = 0;
}

void ASTSTestbench::InitializeRegionsSlot(UTexture2D* Slot)
{
	FRegionsSlot& State = RegionsSlots.FindOrAdd(Slot);
	if (State.bInitialized)
	{
		++Frame.UpdateResourceAfterInit;
		++TotalUpdateResourceAfterInit;
		ensureMsgf(false, TEXT("UpdateResource requested on the initialised Regions slot '%s'."), *Slot->GetName());
		return;
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(STS_UpdateResource);
		Slot->UpdateResource();
	}
	State.bInitialized = true;
	State.Resource = Slot->GetResource();
}

// --- Consumer ---

void ASTSTestbench::SubmitRegionsUpload(FSTSResponse& Response, double NowSec)
{
	FPendingRegionUpload& Pending = UploadQueue.AddDefaulted_GetRef();
	Pending.Request = Response.Request;
	Pending.Source = Response.Source;
	Pending.Buffer = MoveTemp(Response.Buffer);
	Pending.ReleaseTimeSec = NowSec;
}

void ASTSTestbench::ReleaseResponseBuffer(const FSTSRequest& Request, FSTSPayloadBuffer&& Buffer)
{
	const int32 Bucket = STS::ResolutionToBucket(Request.Resolution);
	if (BufferPool.IsValid() && Bucket != INDEX_NONE)
	{
		BufferPool->Release(Bucket, MoveTemp(Buffer));
	}
}

void ASTSTestbench::ApplyRegionsUploads()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(STS_ApplyRegions);
	const bool bRecording = Recorder.IsRecording();

	// Render-thread samples written since last frame, about one frame behind the game thread.
	UploadSamplesScratch.Reset();
	UploadTimer.Drain(UploadSamplesScratch);
	for (const FSTSRenderTimingSample& Sample : UploadSamplesScratch)
	{
		Frame.RTUploadMs += Sample.Ms;
		if (bRecording && Sample.Bucket >= 0 && Sample.Bucket < STS::NumBuckets)
		{
			Window.RTUploadMs[Sample.Bucket].Add(Sample.Ms);
		}
	}
	UploadSamplesScratch.Reset();
	MipTimer.Drain(UploadSamplesScratch);
	if (bRecording)
	{
		for (const FSTSRenderTimingSample& Sample : UploadSamplesScratch)
		{
			Window.MipFit.Add(double(Sample.Bytes), double(Sample.Ms) * 1000.0);
		}
	}
	UploadSamplesScratch.Reset();

	for (int32 Index = InFlightUploads.Num() - 1; Index >= 0; --Index)
	{
		const FUploadTicket& Ticket = *InFlightUploads[Index];
		if (!Ticket.bComplete.load(std::memory_order_acquire))
		{
			continue;
		}
		if (bRecording && Ticket.Bucket != INDEX_NONE)
		{
			const double LatencyMs = FPlatformTime::ToMilliseconds64(Ticket.CompleteCycles.load(std::memory_order_relaxed) - Ticket.IssueCycles);
			Window.UploadLatencyMs[Ticket.Bucket].Add(float(FMath::Max(0.0, LatencyMs)));
		}
		InFlightUploads.RemoveAtSwap(Index);
	}

	if (UploadQueue.IsEmpty())
	{
		return;
	}

	const double StartSec = FPlatformTime::Seconds();
	int32 Kept = 0;
	for (int32 Index = 0; Index < UploadQueue.Num(); ++Index)
	{
		if (!ApplyRegionsUpload(UploadQueue[Index]))
		{
			if (Kept != Index)
			{
				UploadQueue[Kept] = MoveTemp(UploadQueue[Index]);
			}
			++Kept;
		}
	}
	if (Kept < UploadQueue.Num())
	{
		UploadQueue.RemoveAt(Kept, UploadQueue.Num() - Kept);
	}
	Frame.RegionsApplyMs += ElapsedMsSince(StartSec);

	if (bRecording)
	{
		Window.MaxUploadsInFlight = FMath::Max(Window.MaxUploadsInFlight, InFlightUploads.Num());
	}
}

bool ASTSTestbench::ApplyRegionsUpload(FPendingRegionUpload& Pending)
{
	const FSTSRequest& Request = Pending.Request;
	const bool bRecording = Recorder.IsRecording();

	if (Request.TileEpoch != TileEpoch)
	{
		++Frame.Stale;
		++TotalStaleDiscarded;
		if (bRecording)
		{
			++Window.StaleBuildJobs;
		}
		ReleaseResponseBuffer(Request, MoveTemp(Pending.Buffer));
		return true;
	}

	if (!Tiles.IsValidIndex(Request.TileIndex))
	{
		ReleaseResponseBuffer(Request, MoveTemp(Pending.Buffer));
		return true;
	}
	FSTSTile& Tile = Tiles[Request.TileIndex];
	if (Tile.PendingRequestId != Request.RequestId)
	{
		if (bRecording)
		{
			++Window.Superseded;
		}
		ReleaseResponseBuffer(Request, MoveTemp(Pending.Buffer));
		return true;
	}

	const int32 NewBucket = STS::ResolutionToBucket(Request.Resolution);
	const int32 OldBucket = STS::ResolutionToBucket(Tile.Resolution);
	if (NewBucket == INDEX_NONE || OldBucket == INDEX_NONE || !Pending.Source.IsValid() || Pending.Source->Mips.IsEmpty() || Pending.Buffer.Num() != Pending.Source->Data.Num())
	{
		UE_LOG(LogSTS, Error, TEXT("Upload for request %lld is unusable (resolution %d, %d bytes of %d); dropped."), Request.RequestId, Request.Resolution,
			Pending.Buffer.Num(), Pending.Source.IsValid() ? Pending.Source->Data.Num() : 0);
		Tile.PendingRequestId = INDEX_NONE;
		ReleaseResponseBuffer(Request, MoveTemp(Pending.Buffer));
		return true;
	}

	UTexture2D* Slot = TakePooledTexture(NewBucket, Request, Pending.bExhaustedCounted);
	if (!Slot)
	{
		return false;
	}

	const FRegionsSlot* State = RegionsSlots.Find(Slot);
	if (State && State->bInitialized && State->Resource != Slot->GetResource())
	{
		++Frame.UpdateResourceAfterInit;
		++TotalUpdateResourceAfterInit;
		ensureMsgf(false, TEXT("The GPU resource of Regions slot '%s' was replaced after its one UpdateResource."), *Slot->GetName());
	}

	const TSharedRef<FUploadTicket, ESPMode::ThreadSafe> Ticket = MakeShared<FUploadTicket, ESPMode::ThreadSafe>();
	Ticket->Bucket = NewBucket;
	Ticket->IssueCycles = FPlatformTime::Cycles64();

	FSTSUploadOptions Options;
	Options.bPerMipTiming = bPerMipTiming;
	const TSharedPtr<FSTSPayloadBufferPool, ESPMode::ThreadSafe> Pool = BufferPool;

	const double UploadStartSec = FPlatformTime::Seconds();
	const bool bIssued = Uploader->Upload(Slot, Request.Resolution, MoveTemp(Pending.Buffer), Pending.Source->Mips, Options,
		[Ticket, Pool, NewBucket](FSTSPayloadBuffer&& Buffer)
		{
			if (Pool.IsValid())
			{
				Pool->Release(NewBucket, MoveTemp(Buffer));
			}
			Ticket->CompleteCycles.store(FPlatformTime::Cycles64(), std::memory_order_relaxed);
			Ticket->bComplete.store(true, std::memory_order_release);
		});
	Frame.UploadGTMs += ElapsedMsSince(UploadStartSec);

	if (!bIssued)
	{
		TexturePool.Release(Slot, NewBucket, GFrameCounter);
		Tile.PendingRequestId = INDEX_NONE;
		++Frame.UploadFailures;
		++TotalUploadFailures;
		if (LoggedUploadFailures < MaxLoggedUploadFailures)
		{
			++LoggedUploadFailures;
			UE_LOG(LogSTS, Error, TEXT("Upload %lld: the Regions uploader refused slot '%s' (%d); the update is dropped."), Request.RequestId, *Slot->GetName(), Request.Resolution);
		}
		ReleaseResponseBuffer(Request, MoveTemp(Pending.Buffer));
		return true;
	}

	InFlightUploads.Add(Ticket);
	Frame.MipCalls += Pending.Source->Mips.Num();
	CommitTileUpdate(Tile, Slot, Request, OldBucket, NewBucket, Pending.ReleaseTimeSec, Pending.Source->Data.Num());
	return true;
}

// --- Cost model ---

bool ASTSTestbench::SolveCostFit(const FCostFit& Fit, double& OutFixedCostUs, double& OutThroughputGBps, double& OutR2)
{
	if (Fit.N < 3)
	{
		return false;
	}
	const double N = double(Fit.N);
	const double VarianceX = N * Fit.Sxx - Fit.Sx * Fit.Sx;
	const double VarianceY = N * Fit.Syy - Fit.Sy * Fit.Sy;
	if (VarianceX <= 0.0)
	{
		return false;
	}
	const double Covariance = N * Fit.Sxy - Fit.Sx * Fit.Sy;
	const double UsPerByte = Covariance / VarianceX;
	OutFixedCostUs = (Fit.Sy - UsPerByte * Fit.Sx) / N;
	// Bytes per microsecond times 1e-3 is GB/s (1e9 bytes).
	OutThroughputGBps = UsPerByte > 0.0 ? 1.0e-3 / UsPerByte : 0.0;
	OutR2 = VarianceY > 0.0 ? (Covariance * Covariance) / (VarianceX * VarianceY) : 0.0;
	return true;
}

// --- Columns, summary and HUD ---

void ASTSTestbench::FillRegionsColumns(FSTSFrameSample& Sample)
{
	if (BufferPool.IsValid())
	{
		const int64 Misses = BufferPool->GetTotalMisses();
		Frame.BufferPoolMisses = int32(Misses - LastBufferPoolMisses);
		TotalBufferPoolMisses += Frame.BufferPoolMisses;
		LastBufferPoolMisses = Misses;
	}

	const bool bRegions = ActiveUploadMode == ESTSUploadMode::Regions;
	Sample.RTUploadMs = float(Frame.RTUploadMs);
	Sample.UploadGTMs = float(Frame.UploadGTMs);
	Sample.UploadsInFlight = bRegions ? InFlightUploads.Num() : 0;
	Sample.BufferPoolMisses = Frame.BufferPoolMisses;
	Sample.UpdateResourceAfterInit = Frame.UpdateResourceAfterInit;
	float PoolVramTotalMB = 0.0f;
	for (const float MB : PoolVramMB)
	{
		PoolVramTotalMB += MB;
	}
	Sample.PoolVramMB = PoolVramTotalMB;

	if (!Recorder.IsRecording())
	{
		return;
	}
	Window.UpdateResourceAfterInit += Frame.UpdateResourceAfterInit;
	Window.BufferPoolMisses += Frame.BufferPoolMisses;
	Window.UploadFailures += Frame.UploadFailures;
	if (bRegions)
	{
		if (Frame.Uploads > 0)
		{
			Window.UploadGTMs.Add(float(Frame.UploadGTMs));
		}
		Window.MaxMipCallsPerFrame = FMath::Max(Window.MaxMipCallsPerFrame, Frame.MipCalls);
	}
}

void ASTSTestbench::GatherRegionsSummary(TArray<FString>& OutLines) const
{
	if (ActiveUploadMode != ESTSUploadMode::Regions || (Tiles.IsEmpty() && Window.Uploads == 0))
	{
		return;
	}

	OutLines.Add(FString());
	OutLines.Add(TEXT("Regions upload path (Phase 3: one-time UpdateResource per slot, then UpdateTextureRegions per mip from a pooled payload buffer):"));
	OutLines.Add(FString::Printf(TEXT("  Uploader: %s | SafeReuseFrames %d | per-mip timing %s | checksums %s"), Uploader ? Uploader->GetName() : TEXT("none"), SafeReuseFrames,
		bPerMipTiming ? TEXT("ON") : TEXT("off"), bVerifyChecksums ? TEXT("ON (harness cost; not a performance run)") : TEXT("off")));

	float PoolVramTotalMB = 0.0f;
	for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
	{
		PoolVramTotalMB += PoolVramMB[Bucket];
		OutLines.Add(FString::Printf(TEXT("  Slots %-4s: slack %d spares created | in use %d | free %d at stop | nominal VRAM %.1f MB"),
			BucketLabels[Bucket], PoolSlackUsed[Bucket], TexturePool.GetNumInUse(Bucket), TexturePool.GetNumFree(Bucket), PoolVramMB[Bucket]));
	}
	OutLines.Add(FString::Printf(TEXT("  Slot VRAM, all pools: %.1f MB (full mip chain of every slot, tiles and spares; the GPU's own accounting is RHITextureMB / GpuLocalMemMB)"), PoolVramTotalMB));

	if (BufferPool.IsValid())
	{
		for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
		{
			OutLines.Add(FString::Printf(TEXT("  Payload buffers %-4s: capacity %d | free %d at stop | peak outstanding %d | acquires %lld | misses %lld"),
				BucketLabels[Bucket], BufferPool->GetCapacity(Bucket), BufferPool->GetFree(Bucket), BufferPool->GetPeakOutstanding(Bucket), BufferPool->GetAcquires(Bucket), BufferPool->GetMisses(Bucket)));
		}
		OutLines.Add(TEXT("  A miss is a worker allocating a fresh buffer because the pool was empty (workers never wait); the pool size that avoids every miss is the peak outstanding count."));
	}

	OutLines.Add(FString::Printf(TEXT("  Uploads issued %lld (%.1f MB) | unsafe reuses %lld | pool exhausted %lld | stale uploads dropped %lld | upload failures %lld"),
		Window.Uploads, Window.UploadBytes / STS::MegaByte, Window.UnsafeReuses, Window.PoolExhausted, Window.StaleBuildJobs, Window.UploadFailures));
	OutLines.Add(FString::Printf(TEXT("  UpdateResource after the slot's first one: %lld (must be 0) | buffer pool misses %lld | peak uploads in flight %d | peak UpdateTextureRegions calls in a frame %d"),
		Window.UpdateResourceAfterInit, Window.BufferPoolMisses, Window.MaxUploadsInFlight, Window.MaxMipCallsPerFrame));

	if (Window.ReuseAgeFrames.IsEmpty())
	{
		OutLines.Add(TEXT("  Reuse age (frames since release): no reuse of a released slot was recorded"));
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
	OutLines.Add(StatsLine(TEXT("ApplyStepGTMs:"), Window.ApplyStepGTMs));
	OutLines.Add(StatsLine(TEXT("UploadGTMs:"), Window.UploadGTMs));
	OutLines.Add(TEXT("  Per texture by resolution (ms):"));
	for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
	{
		OutLines.Add(StatsLine(*FString::Printf(TEXT("RTUploadMs %s:"), BucketLabels[Bucket]), Window.RTUploadMs[Bucket]));
	}
	OutLines.Add(TEXT("  Upload completion latency, issue -> last mip's cleanup on the RHI thread (ms):"));
	for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
	{
		OutLines.Add(StatsLine(*FString::Printf(TEXT("Latency %s:"), BucketLabels[Bucket]), Window.UploadLatencyMs[Bucket]));
	}

	OutLines.Add(TEXT("  Cost model, time = FixedCost + Bytes / Throughput, per UpdateTextureRegions call (render thread):"));
	double FixedCostUs = 0.0;
	double ThroughputGBps = 0.0;
	double R2 = 0.0;
	if (SolveCostFit(Window.MipFit, FixedCostUs, ThroughputGBps, R2))
	{
		OutLines.Add(FString::Printf(TEXT("    %lld samples | fixed cost %.2f us | throughput %.2f GB/s | R^2 %.3f"), Window.MipFit.N, FixedCostUs, ThroughputGBps, R2));
		OutLines.Add(FString::Printf(TEXT("    Peak %d calls in one frame x %.2f us = %.3f ms of fixed cost in the worst burst frame"),
			Window.MaxMipCallsPerFrame, FixedCostUs, FixedCostUs * Window.MaxMipCallsPerFrame / 1000.0));
		OutLines.Add(TEXT("    Every call is a whole-mip region on the immediate command list, which takes D3D12's lock / memcpy / unlock path (the separate-copy path needs a partial region)."));
	}
	else
	{
		OutLines.Add(bPerMipTiming ? TEXT("    Not enough per-mip samples to fit (needs three calls with different sizes).") : TEXT("    Off: run with -STSPerMipTiming=1 (or bPerMipTiming) to bracket every call; it adds two render commands per mip."));
	}

	if (Window.ReadbackMipsChecked > 0 || TotalReadbackMismatches > 0)
	{
		OutLines.Add(FString::Printf(TEXT("  Readback check: %lld mip(s) compared with the expected payload, %lld mismatch(es)"), Window.ReadbackMipsChecked, Window.ReadbackMipsMismatched));
	}

	OutLines.Add(TEXT("  Regions timing sources:"));
	OutLines.Add(TEXT("    ApplyStepGTMs: FPlatformTime on the game thread over the whole Regions apply step (slot acquire, UpdateTextureRegions calls, commit)."));
	OutLines.Add(TEXT("    UploadGTMs: FPlatformTime on the game thread around the uploader call only (heap allocations and the render-command enqueues of every mip)."));
	OutLines.Add(TEXT("    RTUploadMs: render-thread Cycles64 delta between two render commands around the texture's UpdateTextureRegions calls; read back on the game thread, about 1 frame late."));
	OutLines.Add(TEXT("    Per-mip cost model: the same markers around each single call, with the mip's byte count; only with bPerMipTiming."));
	OutLines.Add(TEXT("    Upload completion latency: FPlatformTime::Cycles64 from the first call to the last mip's DataCleanupFunc, which runs on the RHI thread."));
	OutLines.Add(TEXT("    Retained PlatformData is 0 here by design: the mips are single-use and UpdateResource's one call consumed them."));
}

void ASTSTestbench::AppendRegionsHud(TArray<TPair<FString, FLinearColor>>& Lines) const
{
	if (ActiveUploadMode != ESTSUploadMode::Regions || (Tiles.IsEmpty() && !bCreationInProgress && TotalUploads == 0))
	{
		return;
	}

	FString Pools = TEXT("Slots free / in use (VRAM)");
	for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
	{
		Pools += FString::Printf(TEXT(" | %s: %d / %d (%.0f MB)"), BucketLabels[Bucket], TexturePool.GetNumFree(Bucket), TexturePool.GetNumInUse(Bucket), PoolVramMB[Bucket]);
	}
	Lines.Emplace(Pools, FLinearColor::White);

	if (BufferPool.IsValid())
	{
		FString Buffers = TEXT("Payload buffers free / capacity (peak out)");
		for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
		{
			Buffers += FString::Printf(TEXT(" | %s: %d / %d (%d)"), BucketLabels[Bucket], BufferPool->GetFree(Bucket), BufferPool->GetCapacity(Bucket), BufferPool->GetPeakOutstanding(Bucket));
		}
		Buffers += FString::Printf(TEXT(" | misses %lld"), BufferPool->GetTotalMisses());
		Lines.Emplace(Buffers, BufferPool->GetTotalMisses() > 0 ? FLinearColor::Yellow : FLinearColor::White);
	}

	const bool bProblem = TotalUnsafeReuses > 0 || TotalPoolExhausted > 0 || TotalConsistencyMismatches > 0 || TotalUpdateResourceAfterInit > 0 || TotalUploadFailures > 0 || TotalReadbackMismatches > 0;
	Lines.Emplace(FString::Printf(TEXT("Uploads %lld | queued %d, in flight %d | unsafe reuses %lld | pool exhausted %lld | UpdateResource after init %lld | failures %lld | consistency mismatches %lld | readback mismatches %lld"),
		TotalUploads, UploadQueue.Num(), InFlightUploads.Num(), TotalUnsafeReuses, TotalPoolExhausted, TotalUpdateResourceAfterInit, TotalUploadFailures, TotalConsistencyMismatches, TotalReadbackMismatches),
		bProblem ? FLinearColor::Red : FLinearColor::White);
}

// --- GPU readback ---

void ASTSTestbench::CheckReadbackMips()
{
	if (!IsGameWorldValid(TEXT("CheckReadbackMips")))
	{
		return;
	}
	RunReadbackMips(0);
}

int32 ASTSTestbench::RunReadbackMips(int32 OnlyResolution)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(STS_ReadbackMips);

	if (!STS::UpdatesVisuals(ActiveUploadMode))
	{
		UE_LOG(LogSTS, Warning, TEXT("Readback check: mode %s never updates the visuals; switch to Baseline, BaselineSingleUse or Regions."), *STS::EnumToString(ActiveUploadMode));
		return 0;
	}
	if (!IsUploadPathIdle())
	{
		UE_LOG(LogSTS, Warning, TEXT("Readback check: uploads are still queued or in flight; tiles with an update pending are skipped."));
	}

	TArray<FReadbackJob> Jobs;
	int32 Skipped = 0;
	for (int32 Bucket = STS::NumBuckets - 1; Bucket >= 0; --Bucket)
	{
		const int32 Resolution = STS::BucketResolutions[Bucket];
		if (OnlyResolution > 0 && OnlyResolution != Resolution)
		{
			continue;
		}

		// Tiles that received an update come first: they are what the uploader wrote.
		TArray<int32> Candidates;
		for (int32 TileIndex = 0; TileIndex < Tiles.Num(); ++TileIndex)
		{
			const FSTSTile& Tile = Tiles[TileIndex];
			if (Tile.Resolution == Resolution && Tile.PendingRequestId == INDEX_NONE && Tile.Texture && Tile.Texture->GetResource())
			{
				Candidates.Add(TileIndex);
			}
		}
		Candidates.StableSort([this](int32 A, int32 B) { return (Tiles[A].UpdateCount > 0) && !(Tiles[B].UpdateCount > 0); });

		const int32 Count = FMath::Min(Candidates.Num(), FMath::Max(1, ReadbackSlotsPerResolution));
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FSTSTile& Tile = Tiles[Candidates[Index]];
			TSharedPtr<const FSTSPayload> Expected;
			if (ServerLibrary.IsValid())
			{
				Expected = ServerLibrary->Find(Tile.Resolution, Tile.VisualVariant);
			}
			if (!Expected.IsValid() && Tile.VisualVariant == PayloadLibraryVariant)
			{
				if (const TSharedPtr<const FSTSMipChain>* Chain = PayloadLibrary.Find(Tile.Resolution))
				{
					Expected = FSTSPayload::FromChain(Tile.Resolution, Tile.VisualVariant, **Chain);
				}
			}
			if (!Expected.IsValid())
			{
				++Skipped;
				UE_LOG(LogSTS, Warning, TEXT("Readback check: no expected payload for tile %d (%d, variant %d); skipped."), Candidates[Index], Tile.Resolution, Tile.VisualVariant);
				continue;
			}

			FReadbackJob& Job = Jobs.AddDefaulted_GetRef();
			Job.Label = FString::Printf(TEXT("tile %d (%d, variant %d, %d update(s))"), Candidates[Index], Tile.Resolution, Tile.VisualVariant, Tile.UpdateCount);
			Job.Resource = Tile.Texture->GetResource();
			Job.Expected = Expected;
		}
	}

	if (Jobs.IsEmpty())
	{
		UE_LOG(LogSTS, Warning, TEXT("Readback check: no tile to check (%d skipped)."), Skipped);
		return 0;
	}

	const TSharedRef<FReadbackResult, ESPMode::ThreadSafe> Result = MakeShared<FReadbackResult, ESPMode::ThreadSafe>();
	const double StartSec = FPlatformTime::Seconds();
	ENQUEUE_RENDER_COMMAND(STSReadbackMips)([Jobs, Result](FRHICommandListImmediate& RHICmdList)
	{
		for (const FReadbackJob& Job : Jobs)
		{
			FRHITexture* Texture = Job.Resource ? Job.Resource->TextureRHI.GetReference() : nullptr;
			if (!Texture)
			{
				++Result->MipsMismatched;
				Result->Lines.Add(FString::Printf(TEXT("%s: no RHI texture"), *Job.Label));
				continue;
			}

			const FSTSPayload& Expected = *Job.Expected;
			const int32 NumMips = int32(Texture->GetNumMips());
			if (NumMips != Expected.Mips.Num())
			{
				++Result->MipsMismatched;
				Result->Lines.Add(FString::Printf(TEXT("%s: texture has %d mips, payload %d"), *Job.Label, NumMips, Expected.Mips.Num()));
			}

			for (int32 MipIndex = 0; MipIndex < FMath::Min(NumMips, Expected.Mips.Num()); ++MipIndex)
			{
				const int32 MipSize = FMath::Max(1, Expected.Resolution >> MipIndex);
				const int64 BlocksPerRow = FMath::Max(1, (MipSize + 3) / 4);
				const int64 RowBytes = BlocksPerRow * DxtBlockBytes;
				const FSTSMipRange& Range = Expected.Mips[MipIndex];
				const int64 Rows = Range.Size / RowBytes;
				++Result->MipsChecked;
				if (Rows * RowBytes != Range.Size)
				{
					++Result->MipsMismatched;
					Result->Lines.Add(FString::Printf(TEXT("%s mip %d: payload range of %lld bytes is not a whole number of %lld-byte rows"), *Job.Label, MipIndex, Range.Size, RowBytes));
					continue;
				}

				const FRHILockTextureResult Locked = RHICmdList.LockTexture(FRHILockTextureArgs::Lock2D(Texture, MipIndex, RLM_ReadOnly, false));
				if (!Locked.Data)
				{
					++Result->MipsMismatched;
					Result->Lines.Add(FString::Printf(TEXT("%s mip %d: lock failed"), *Job.Label, MipIndex));
					continue;
				}

				int64 BadRows = 0;
				int64 FirstBadRow = INDEX_NONE;
				for (int64 Row = 0; Row < Rows; ++Row)
				{
					const uint8* Actual = static_cast<const uint8*>(Locked.Data) + Row * Locked.Stride;
					const uint8* Wanted = Expected.Data.GetData() + Range.Offset + Row * RowBytes;
					if (FMemory::Memcmp(Actual, Wanted, RowBytes) != 0)
					{
						++BadRows;
						if (FirstBadRow == INDEX_NONE)
						{
							FirstBadRow = Row;
						}
					}
				}
				RHICmdList.UnlockTexture(FRHILockTextureArgs::Lock2D(Texture, MipIndex, RLM_Num, false));

				if (BadRows > 0)
				{
					++Result->MipsMismatched;
					Result->Lines.Add(FString::Printf(TEXT("%s mip %d (%dx%d): %lld of %lld block rows differ, first at row %lld"), *Job.Label, MipIndex, MipSize, MipSize, BadRows, Rows, FirstBadRow));
				}
			}
		}
	});
	FlushRenderingCommands();

	Window.ReadbackMipsChecked += Result->MipsChecked;
	Window.ReadbackMipsMismatched += Result->MipsMismatched;
	TotalReadbackMismatches += Result->MipsMismatched;

	UE_LOG(LogSTS, Log, TEXT("Readback check (%s): %d tile(s), %d mip(s) compared, %d mismatch(es), %d skipped, in %.1f ms."),
		*STS::EnumToString(ActiveUploadMode), Jobs.Num(), Result->MipsChecked, Result->MipsMismatched, Skipped, ElapsedMsSince(StartSec));
	for (int32 Index = 0; Index < FMath::Min(Result->Lines.Num(), MaxReportedReadbackMismatches); ++Index)
	{
		UE_LOG(LogSTS, Error, TEXT("  Readback mismatch: %s"), *Result->Lines[Index]);
	}
	if (Result->Lines.Num() > MaxReportedReadbackMismatches)
	{
		UE_LOG(LogSTS, Error, TEXT("  ... %d more not listed"), Result->Lines.Num() - MaxReportedReadbackMismatches);
	}
	return Result->MipsMismatched;
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSTextureUploader.h"

#include "SimTextureStrm.h"
#include "Engine/Texture2D.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "STSDxt1Generator.h"
#include "TextureResource.h"

// --- Payload buffer pool ---

FSTSPayloadBufferPool::FSTSPayloadBufferPool(const int32 (&InCapacity)[STS::NumBuckets])
{
	for (int32 Bucket = 0; Bucket < STS::NumBuckets; ++Bucket)
	{
		Capacity[Bucket] = FMath::Max(0, InCapacity[Bucket]);
		Acquires[Bucket].store(0);
		Misses[Bucket].store(0);
		Outstanding[Bucket].store(0);
		PeakOutstanding[Bucket].store(0);

		const int32 ChainBytes = int32(FSTSDxt1Generator::GetChainBytes(STS::BucketResolutions[Bucket], STS::BucketResolutions[Bucket]));
		FreeBuffers[Bucket].Reserve(Capacity[Bucket]);
		for (int32 Index = 0; Index < Capacity[Bucket]; ++Index)
		{
			FSTSPayloadBuffer& Buffer = FreeBuffers[Bucket].AddDefaulted_GetRef();
			Buffer.SetNumUninitialized(ChainBytes);
		}
	}
}

FSTSPayloadBuffer FSTSPayloadBufferPool::Acquire(int32 Bucket, int64 NumBytes)
{
	FSTSPayloadBuffer Buffer;
	bool bFromPool = false;
	{
		FScopeLock ScopeLock(&Lock);
		if (!FreeBuffers[Bucket].IsEmpty())
		{
			Buffer = FreeBuffers[Bucket].Pop(EAllowShrinking::No);
			bFromPool = true;
		}
	}

	Acquires[Bucket].fetch_add(1, std::memory_order_relaxed);
	if (!bFromPool)
	{
		Misses[Bucket].fetch_add(1, std::memory_order_relaxed);
	}
	const int32 Now = Outstanding[Bucket].fetch_add(1, std::memory_order_relaxed) + 1;
	int32 Peak = PeakOutstanding[Bucket].load(std::memory_order_relaxed);
	while (Now > Peak && !PeakOutstanding[Bucket].compare_exchange_weak(Peak, Now, std::memory_order_relaxed))
	{
	}

	Buffer.SetNumUninitialized(int32(NumBytes), EAllowShrinking::No);
	return Buffer;
}

void FSTSPayloadBufferPool::Release(int32 Bucket, FSTSPayloadBuffer&& Buffer)
{
	if (Buffer.GetAllocatedSize() == 0)
	{
		return;
	}
	Outstanding[Bucket].fetch_sub(1, std::memory_order_relaxed);
	FScopeLock ScopeLock(&Lock);
	FreeBuffers[Bucket].Add(MoveTemp(Buffer));
}

int32 FSTSPayloadBufferPool::GetFree(int32 Bucket) const
{
	FScopeLock ScopeLock(&Lock);
	return FreeBuffers[Bucket].Num();
}

int64 FSTSPayloadBufferPool::GetTotalMisses() const
{
	int64 Total = 0;
	for (const std::atomic<int64>& Count : Misses)
	{
		Total += Count.load(std::memory_order_relaxed);
	}
	return Total;
}

int64 FSTSPayloadBufferPool::GetPooledBytes() const
{
	FScopeLock ScopeLock(&Lock);
	int64 Total = 0;
	for (const TArray<FSTSPayloadBuffer>& Buffers : FreeBuffers)
	{
		for (const FSTSPayloadBuffer& Buffer : Buffers)
		{
			Total += Buffer.GetAllocatedSize();
		}
	}
	return Total;
}

// --- Regions uploader ---

namespace
{
	/** Owns the response buffer until the cleanup of the last mip has run; the cleanups run on the RHI thread. */
	struct FUploadBatch
	{
		FSTSPayloadBuffer Buffer;
		FSTSUploadCompleteFunc OnComplete;
		std::atomic<int32> MipsPending{ 0 };

		void MipConsumed()
		{
			if (MipsPending.fetch_sub(1, std::memory_order_acq_rel) == 1)
			{
				OnComplete(MoveTemp(Buffer));
			}
		}
	};
}

FSTSRegionsUploader::FSTSRegionsUploader(const FSTSRenderTimer& InUploadTimer, const FSTSRenderTimer& InMipTimer)
	: UploadTimer(InUploadTimer)
	, MipTimer(InMipTimer)
{
}

bool FSTSRegionsUploader::Upload(UTexture2D* Slot, int32 Resolution, FSTSPayloadBuffer&& Buffer, TConstArrayView<FSTSMipRange> Mips, const FSTSUploadOptions& Options, FSTSUploadCompleteFunc&& OnComplete)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(STS_UploadIssue);

	// UpdateTextureRegions returns without running the cleanup in these cases, which would leak the buffer and never complete.
	FTextureResource* Resource = Slot ? Slot->GetResource() : nullptr;
	if (!Resource || !Resource->GetTexture2DResource() || Slot->IsCurrentlyVirtualTextured() || Slot->IsStreamable()
		|| int32(Resource->GetSizeX()) != Resolution || Mips.IsEmpty())
	{
		return false;
	}

	const int32 Bucket = STS::ResolutionToBucket(Resolution);
	TSharedRef<FUploadBatch, ESPMode::ThreadSafe> Batch = MakeShared<FUploadBatch, ESPMode::ThreadSafe>();
	Batch->Buffer = MoveTemp(Buffer);
	Batch->OnComplete = MoveTemp(OnComplete);
	Batch->MipsPending.store(Mips.Num(), std::memory_order_relaxed);

	UploadTimer.EnqueueBegin();
	for (int32 MipIndex = 0; MipIndex < Mips.Num(); ++MipIndex)
	{
		const int32 MipSize = FMath::Max(1, Resolution >> MipIndex);

		// FD3D12Texture::UpdateTexture2D wants whole 4x4 blocks, so the 2x2 and 1x1 tail mips are padded to one block. A DXT1 block row is 8 bytes per block.
		const uint32 PaddedSize = uint32(Align(MipSize, 4));
		const uint32 SrcPitch = uint32(FMath::Max(1, MipSize / 4) * 8);
		const FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, PaddedSize, PaddedSize);
		uint8* SrcData = Batch->Buffer.GetData() + Mips[MipIndex].Offset;

		const auto IssueMip = [&]()
		{
			// SrcBpp only scales SrcX, which is 0; 8 is the DXT1 block size.
			Slot->UpdateTextureRegions(MipIndex, 1, Region, SrcPitch, 8, SrcData,
				[Batch](uint8*, const FUpdateTextureRegion2D* InRegion)
				{
					delete InRegion;
					Batch->MipConsumed();
				});
		};

		if (Options.bPerMipTiming)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(STS_UpdateTextureRegionsMip);
			MipTimer.EnqueueBegin();
			IssueMip();
			MipTimer.EnqueueEnd(Bucket, Mips[MipIndex].Size, MipIndex);
		}
		else
		{
			IssueMip();
		}
	}
	UploadTimer.EnqueueEnd(Bucket);
	return true;
}

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "STSRenderTiming.h"
#include "STSTerrainServer.h"
#include "STSTypes.h"

class UTexture2D;

/**
 * Per-resolution pool of response buffers. Server workers take from it and the last upload cleanup returns to it (on the RHI thread), so every method is thread-safe.
 * Acquire never waits for a buffer: when the pool is empty it hands out a fresh allocation and counts a miss; the extra buffer joins the pool when it is released.
 */
class FSTSPayloadBufferPool
{
public:
	/** Creates Capacity[Bucket] buffers of one full mip chain of that resolution. Memory is reserved, not touched. */
	explicit FSTSPayloadBufferPool(const int32 (&InCapacity)[STS::NumBuckets]);

	UE_NONCOPYABLE(FSTSPayloadBufferPool);

	/** Returns a buffer whose Num() is NumBytes. */
	FSTSPayloadBuffer Acquire(int32 Bucket, int64 NumBytes);

	/** Returns a buffer to its bucket. Empty buffers are ignored. */
	void Release(int32 Bucket, FSTSPayloadBuffer&& Buffer);

	int32 GetCapacity(int32 Bucket) const { return Capacity[Bucket]; }
	int32 GetFree(int32 Bucket) const;
	int64 GetMisses(int32 Bucket) const { return Misses[Bucket].load(std::memory_order_relaxed); }
	int64 GetTotalMisses() const;
	int64 GetAcquires(int32 Bucket) const { return Acquires[Bucket].load(std::memory_order_relaxed); }
	/** Most buffers out at once, since creation. The pool size needed to avoid every miss. */
	int32 GetPeakOutstanding(int32 Bucket) const { return PeakOutstanding[Bucket].load(std::memory_order_relaxed); }
	/** Reserved memory of the buffers currently held by the pool, in bytes. */
	int64 GetPooledBytes() const;

private:
	int32 Capacity[STS::NumBuckets] = {};
	mutable FCriticalSection Lock;
	TArray<FSTSPayloadBuffer> FreeBuffers[STS::NumBuckets];
	std::atomic<int64> Acquires[STS::NumBuckets];
	std::atomic<int64> Misses[STS::NumBuckets];
	std::atomic<int32> Outstanding[STS::NumBuckets];
	std::atomic<int32> PeakOutstanding[STS::NumBuckets];
};

struct FSTSUploadOptions
{
	/** Wraps each mip's call in its own render-thread markers and trace scope (cost-model measurement). */
	bool bPerMipTiming = false;
};

/** Runs on any thread once the last mip's source data is no longer needed; hands the buffer back to its owner. */
using FSTSUploadCompleteFunc = TFunction<void(FSTSPayloadBuffer&&)>;

/** Puts a payload into a texture that already has a GPU resource. Phase 4 adds a batched custom uploader behind the same interface. */
class ISTSTextureUploader
{
public:
	virtual ~ISTSTextureUploader() = default;

	virtual const TCHAR* GetName() const = 0;

	/**
	 * Game thread. Queues every mip of Buffer (laid out as Mips says) into Slot, and takes ownership of Buffer.
	 * OnComplete is called exactly once, on any thread, after the last mip was consumed.
	 * Returns false when the slot cannot be updated; Buffer is then untouched and OnComplete is never called.
	 */
	virtual bool Upload(UTexture2D* Slot, int32 Resolution, FSTSPayloadBuffer&& Buffer, TConstArrayView<FSTSMipRange> Mips, const FSTSUploadOptions& Options, FSTSUploadCompleteFunc&& OnComplete) = 0;
};

/**
 * One UTexture2D::UpdateTextureRegions call per mip, each covering the whole mip (padded to a multiple of 4 texels) from (0, 0).
 * The render-thread timers bracket the whole texture (UploadTimer) and, with bPerMipTiming, each mip's call (MipTimer).
 */
class FSTSRegionsUploader final : public ISTSTextureUploader
{
public:
	FSTSRegionsUploader(const FSTSRenderTimer& InUploadTimer, const FSTSRenderTimer& InMipTimer);

	virtual const TCHAR* GetName() const override { return TEXT("Regions"); }
	virtual bool Upload(UTexture2D* Slot, int32 Resolution, FSTSPayloadBuffer&& Buffer, TConstArrayView<FSTSMipRange> Mips, const FSTSUploadOptions& Options, FSTSUploadCompleteFunc&& OnComplete) override;

private:
	FSTSRenderTimer UploadTimer;
	FSTSRenderTimer MipTimer;
};

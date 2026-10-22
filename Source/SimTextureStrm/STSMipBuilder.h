// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Deque.h"
#include "Containers/MpscQueue.h"
#include "HAL/CriticalSection.h"
#include "STSTerrainServer.h"
#include "TextureResource.h"

/** One response on its way to a texture. The builder fills Mips from Buffer; the game thread hands them to PlatformData. */
struct FSTSBuildJob
{
	FSTSRequest Request;
	/** Gives the mip offsets and sizes of Buffer. */
	TSharedPtr<const FSTSPayload> Source;
	/** The response buffer; freed by the builder once the mips are built. */
	FSTSPayloadBuffer Buffer;
	/** Game-thread time the response was released by the server delay line. */
	double ReleaseTimeSec = 0.0;

	/** Owned until the game thread transfers them with Mips.Add(); a dropped job frees them. */
	TArray<TUniquePtr<FTexture2DMipMap>> Mips;
	double BuildMs = 0.0;
	/** The builder skipped the work because the epoch was already stale. */
	bool bBuildSkipped = false;
	/** Game thread only: PoolExhausted was already counted for this job. */
	bool bExhaustedCounted = false;
};

/**
 * Background mip builder that mimics the product: dedicated threads, a new FTexture2DMipMap per mip, and
 * BulkData Lock(LOCK_READ_WRITE) -> Realloc -> memcpy -> Unlock. Threads never touch a UObject.
 * Every method except the worker loop is game-thread only.
 */
class FSTSMipBuilder
{
public:
	explicit FSTSMipBuilder(int32 InThreadCount);
	~FSTSMipBuilder();

	UE_NONCOPYABLE(FSTSMipBuilder);

	/** Jobs of any other epoch are not built; they still come back through Drain so the game thread can count and free them. */
	void SetEpoch(uint32 NewEpoch);

	void Submit(FSTSBuildJob&& Job);

	/** Appends the finished jobs in completion order. */
	void Drain(TArray<FSTSBuildJob>& OutFinished);

	/** Stops the workers and frees everything still queued or finished. */
	void Shutdown();

	int32 GetThreadCount() const { return ThreadCount; }

	/** Submitted jobs not yet drained: queued, being built, or finished and waiting for Drain. */
	int32 GetOutstanding() const { return Outstanding; }

private:
	class FWorker;

	bool TryTakeJob(FSTSBuildJob& OutJob);
	void Process(FSTSBuildJob& Job);

	int32 ThreadCount = 0;
	std::atomic<uint32> Epoch{ 0 };
	std::atomic<bool> bStopping{ false };
	int32 Outstanding = 0;

	FCriticalSection JobLock;
	TDeque<FSTSBuildJob> Jobs;
	TMpscQueue<FSTSBuildJob> Finished;
	TArray<TUniquePtr<FWorker>> Workers;
};

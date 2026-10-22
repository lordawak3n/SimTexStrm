// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/MpscQueue.h"

struct FSTSRenderTimingSample
{
	int32 Bucket = INDEX_NONE;
	float Ms = 0.0f;
};

/**
 * Brackets a game-thread UpdateResource() with two render commands. The render thread runs commands in order, so everything
 * UpdateResource enqueues (the old resource's deletion, the new resource's InitRHI) executes between the two timestamps.
 * Samples are produced on the render thread and read back on the game thread, so they arrive about one frame late.
 * The shared state outlives the owner, because render commands can still run after it is destroyed.
 */
class FSTSRenderTimer
{
public:
	FSTSRenderTimer();

	/** Game thread. Call immediately before UpdateResource. */
	void EnqueueBegin() const;

	/** Game thread. Call immediately after UpdateResource; Bucket tags the sample with its resolution. */
	void EnqueueEnd(int32 Bucket) const;

	/** Game thread. Appends every sample the render thread has produced since the last call. */
	void Drain(TArray<FSTSRenderTimingSample>& OutSamples) const;

private:
	struct FState
	{
		/** Written and read on the render thread only. */
		uint64 BeginCycles = 0;
		TMpscQueue<FSTSRenderTimingSample> Samples;
	};

	TSharedRef<FState, ESPMode::ThreadSafe> State;
};

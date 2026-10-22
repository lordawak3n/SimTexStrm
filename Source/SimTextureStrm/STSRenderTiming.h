// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/MpscQueue.h"

struct FSTSRenderTimingSample
{
	int32 Bucket = INDEX_NONE;
	float Ms = 0.0f;
	/** Per-mip markers only: the bytes the marked call moved and the mip it moved them to. */
	int64 Bytes = 0;
	int32 MipIndex = INDEX_NONE;
};

/**
 * Brackets game-thread calls (UpdateResource, or the UpdateTextureRegions calls of one upload) with two render commands. The render thread runs commands in order,
 * so everything the bracketed calls enqueue executes between the two timestamps.
 * Samples are produced on the render thread and read back on the game thread, so they arrive about one frame late.
 * The shared state outlives the owner, because render commands can still run after it is destroyed.
 * Begin/End pairs of one timer must not nest; use one timer per nesting level.
 */
class FSTSRenderTimer
{
public:
	FSTSRenderTimer();

	/** Game thread. Call immediately before the bracketed work. */
	void EnqueueBegin() const;

	/** Game thread. Call immediately after the bracketed work; Bucket tags the sample with its resolution. */
	void EnqueueEnd(int32 Bucket, int64 Bytes = 0, int32 MipIndex = INDEX_NONE) const;

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

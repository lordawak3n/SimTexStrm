// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSRenderTiming.h"

#include "HAL/PlatformTime.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

FSTSRenderTimer::FSTSRenderTimer()
	: State(MakeShared<FState, ESPMode::ThreadSafe>())
{
}

void FSTSRenderTimer::EnqueueBegin() const
{
	ENQUEUE_RENDER_COMMAND(STSRenderTimerBegin)([State = State](FRHICommandListImmediate&)
	{
		State->BeginCycles = FPlatformTime::Cycles64();
	});
}

void FSTSRenderTimer::EnqueueEnd(int32 Bucket, int64 Bytes, int32 MipIndex) const
{
	ENQUEUE_RENDER_COMMAND(STSRenderTimerEnd)([State = State, Bucket, Bytes, MipIndex](FRHICommandListImmediate&)
	{
		FSTSRenderTimingSample Sample;
		Sample.Bucket = Bucket;
		Sample.Ms = float(FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - State->BeginCycles));
		Sample.Bytes = Bytes;
		Sample.MipIndex = MipIndex;
		State->Samples.Enqueue(Sample);
	});
}

void FSTSRenderTimer::Drain(TArray<FSTSRenderTimingSample>& OutSamples) const
{
	FSTSRenderTimingSample Sample;
	while (State->Samples.Dequeue(Sample))
	{
		OutSamples.Add(Sample);
	}
}

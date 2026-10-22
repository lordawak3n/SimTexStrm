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
	ENQUEUE_RENDER_COMMAND(STSUpdateResourceBegin)([State = State](FRHICommandListImmediate&)
	{
		State->BeginCycles = FPlatformTime::Cycles64();
	});
}

void FSTSRenderTimer::EnqueueEnd(int32 Bucket) const
{
	ENQUEUE_RENDER_COMMAND(STSUpdateResourceEnd)([State = State, Bucket](FRHICommandListImmediate&)
	{
		FSTSRenderTimingSample Sample;
		Sample.Bucket = Bucket;
		Sample.Ms = float(FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - State->BeginCycles));
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

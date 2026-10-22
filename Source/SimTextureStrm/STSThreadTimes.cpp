// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSThreadTimes.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformAtomics.h"
#include "Rendering/SlateRenderer.h"
#include "RHICommandList.h"
#include "RenderTimer.h"

struct FSTSThreadTimeMonitor::FState
{
	// Render thread only.
	uint64 CurrentFrame = MAX_uint64;
	int32 PresentsThisFrame = 0;

	std::atomic<uint32> RenderCycles{ 0 };
	std::atomic<uint32> RHICycles{ 0 };
	std::atomic<int32> LastFramePresents{ 0 };
};

FSTSThreadTimeMonitor::FSTSThreadTimeMonitor()
	: State(MakeShared<FState, ESPMode::ThreadSafe>())
{
}

FSTSThreadTimeMonitor::~FSTSThreadTimeMonitor()
{
	Stop();
}

bool FSTSThreadTimeMonitor::Start()
{
	if (Handle.IsValid())
	{
		return true;
	}
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}
	FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer();
	if (!Renderer)
	{
		return false;
	}

	State->CurrentFrame = MAX_uint64;
	State->PresentsThisFrame = 0;
	Handle = Renderer->OnBackBufferReadyToPresent().AddLambda([State = State](SWindow&, const FTextureRHIRef&)
	{
		// Render thread, at the start of a present: the globals still hold the result of the previous present.
		bool bCaptureRHI = false;
		const uint64 Frame = GFrameCounterRenderThread;
		if (Frame != State->CurrentFrame)
		{
			// First present of a frame. If the previous frame had a single present, its result is what the globals hold now.
			if (State->PresentsThisFrame == 1)
			{
				State->RenderCycles.store(GRenderThreadTime, std::memory_order_relaxed);
				bCaptureRHI = true;
			}
			State->LastFramePresents.store(State->PresentsThisFrame, std::memory_order_relaxed);
			State->CurrentFrame = Frame;
			State->PresentsThisFrame = 1;
		}
		else if (++State->PresentsThisFrame == 2)
		{
			// The globals hold the first present's result, which measured the whole frame.
			State->RenderCycles.store(GRenderThreadTime, std::memory_order_relaxed);
			bCaptureRHI = true;
		}

		if (bCaptureRHI)
		{
			if (IsRunningRHIInSeparateThread())
			{
				FRHICommandListExecutor::GetImmediateCommandList().EnqueueLambda([State](FRHICommandListImmediate&)
				{
					const int32 Value = FPlatformAtomics::AtomicRead(reinterpret_cast<volatile const int32*>(&GRHIThreadTime));
					State->RHICycles.store(uint32(Value), std::memory_order_relaxed);
				});
			}
			else
			{
				State->RHICycles.store(0, std::memory_order_relaxed);
			}
		}
	});
	return true;
}

void FSTSThreadTimeMonitor::Stop()
{
	if (!Handle.IsValid())
	{
		return;
	}
	if (FSlateApplication::IsInitialized())
	{
		if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
		{
			Renderer->OnBackBufferReadyToPresent().Remove(Handle);
		}
	}
	Handle.Reset();
}

FSTSThreadTimeReading FSTSThreadTimeMonitor::Read() const
{
	FSTSThreadTimeReading Reading;
	Reading.RenderCycles = State->RenderCycles.load(std::memory_order_relaxed);
	Reading.RHICycles = State->RHICycles.load(std::memory_order_relaxed);
	Reading.Presents = State->LastFramePresents.load(std::memory_order_relaxed);
	return Reading;
}

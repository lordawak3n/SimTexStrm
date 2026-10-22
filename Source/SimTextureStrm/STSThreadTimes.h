// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** The latest per-frame thread times published by FSTSThreadTimeMonitor, in platform cycles. */
struct FSTSThreadTimeReading
{
	uint32 RenderCycles = 0;
	/** 0 when the RHI does not run in a separate thread. */
	uint32 RHICycles = 0;
	/** Slate windows presented in the last completed frame. */
	int32 Presents = 0;
};

/**
 * Reads the render and RHI thread busy time of the frame from the right Slate present.
 *
 * FSlateRHIRenderer::PresentWindow_RenderThread recomputes GRenderThreadTime and GRHIThreadTime on every window it
 * presents, as the time since the previous present. With two or more windows per frame (normal in PIE and the editor)
 * the globals end up holding the few microseconds between the last two back-to-back presents, not the frame; stat unit reads the same globals.
 * The first present of a frame measures the whole frame, so this reads the globals right after that present (before the second one
 * overwrites them), or at the start of the next frame when only one window is presented. The delegate runs on the render thread;
 * the RHI value is read by a lambda enqueued at the same point, so it runs on the RHI thread in order with the engine's own updates.
 */
class FSTSThreadTimeMonitor
{
public:
	FSTSThreadTimeMonitor();
	~FSTSThreadTimeMonitor();

	UE_NONCOPYABLE(FSTSThreadTimeMonitor);

	/** Game thread. Returns false when there is no Slate renderer (the engine globals are then all there is). */
	bool Start();
	void Stop();
	bool IsRunning() const { return Handle.IsValid(); }

	FSTSThreadTimeReading Read() const;

private:
	struct FState;

	TSharedRef<FState, ESPMode::ThreadSafe> State;
	FDelegateHandle Handle;
};

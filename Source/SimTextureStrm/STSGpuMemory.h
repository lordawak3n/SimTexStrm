// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace STSGpuMemory
{
	/**
	 * This process's GPU local (VRAM) memory in use, in MB, from DXGI QueryVideoMemoryInfo(LOCAL).CurrentUsage on the
	 * adapter the D3D12 RHI is using. That is the same call FD3D12Adapter::CollectMemoryStats makes, which the RHI only
	 * publishes to stats and the CSV profiler. Returns a negative value when unavailable (not D3D12, or the query failed).
	 * Game thread only.
	 */
	float QueryLocalUsageMB();
}

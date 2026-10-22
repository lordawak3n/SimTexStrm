// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSGpuMemory.h"

#include "RHI.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <d3d12.h>
#include <dxgi1_4.h>
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace STSGpuMemory
{
#if PLATFORM_WINDOWS
	namespace
	{
		TRefCountPtr<IDXGIAdapter3> RhiAdapter;
		bool bAdapterLookupDone = false;

		void FindRhiAdapter()
		{
			bAdapterLookupDone = true;
			if (!GDynamicRHI || RHIGetInterfaceType() != ERHIInterfaceType::D3D12)
			{
				return;
			}

			ID3D12Device* Device = static_cast<ID3D12Device*>(GDynamicRHI->RHIGetNativeDevice());
			if (!Device)
			{
				return;
			}

			TRefCountPtr<IDXGIFactory4> Factory;
			if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(Factory.GetInitReference()))))
			{
				Factory->EnumAdapterByLuid(Device->GetAdapterLuid(), IID_PPV_ARGS(RhiAdapter.GetInitReference()));
			}
		}
	}
#endif

	float QueryLocalUsageMB()
	{
#if PLATFORM_WINDOWS
		if (!bAdapterLookupDone)
		{
			FindRhiAdapter();
		}
		if (RhiAdapter)
		{
			DXGI_QUERY_VIDEO_MEMORY_INFO Info = {};
			if (SUCCEEDED(RhiAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Info)))
			{
				return float(double(Info.CurrentUsage) / (1024.0 * 1024.0));
			}
		}
#endif
		return -1.0f;
	}
}

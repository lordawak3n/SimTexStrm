// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "STSTypes.generated.h"

UENUM(BlueprintType)
enum class ESTSMipCoding : uint8
{
	None,
	Brightness,
	Checker
};

UENUM(BlueprintType)
enum class ESTSCameraPreset : uint8
{
	Oblique,
	TopDown,
	Custom
};

namespace STS
{
	/** Resolution buckets in ascending order; the index matches the TexMB_* CSV columns. */
	constexpr int32 NumBuckets = 4;
	constexpr int32 BucketResolutions[NumBuckets] = { 512, 1024, 2048, 4096 };

	inline int32 ResolutionToBucket(int32 Resolution)
	{
		for (int32 Bucket = 0; Bucket < NumBuckets; ++Bucket)
		{
			if (BucketResolutions[Bucket] == Resolution)
			{
				return Bucket;
			}
		}
		return INDEX_NONE;
	}

	constexpr double MegaByte = 1024.0 * 1024.0;
}

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Class.h"
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

/** How an update changes a tile's logical resolution and variant. */
UENUM(BlueprintType)
enum class ESTSUpdatePattern : uint8
{
	/** Same resolution, next content variant. */
	SameResolution,
	/** One power-of-two step up, clamped to 4096. */
	StepUp,
	/** One power-of-two step down, clamped to 512. */
	StepDown,
	/** Weighted choice between the three patterns above, among those that can be satisfied. */
	Mixed,
	/** Tiles alternate between a resolution pair every trigger. Counts address the pair by its larger resolution. */
	Oscillate
};

UENUM(BlueprintType)
enum class ESTSImpossibleStepPolicy : uint8
{
	/** Only tiles that can make the step are selected; a shortfall is reported. */
	PickAnotherTile,
	FallbackToSameResolution,
	/** The request is dropped and counted as skipped. */
	Skip
};

UENUM(BlueprintType)
enum class ESTSTargetMode : uint8
{
	Random,
	RankRange,
	Patch
};

UENUM(BlueprintType)
enum class ESTSScenarioStepType : uint8
{
	Wait,
	WaitForIdle,
	CreateTiles,
	DestroyTiles,
	RecordStart,
	RecordStop,
	SetPattern,
	SetTargetMode,
	SetSeed,
	Burst,
	SpreadStart,
	SpreadStop,
	/** Repeats the burst counts Rate times per second for DurationSec (timeline-based). */
	BurstRepeat,
	Quit
};

UENUM(BlueprintType)
enum class ESTSScenarioPreset : uint8
{
	Custom,
	SteadySpread,
	SpreadSweep,
	Burst,
	Oscillation,
	Soak,
	/** Not in the spec: idle versus 50/s spread recordings for measuring the harness cost. */
	Overhead,
	/** Not in the spec: destroys and rebuilds the tiles while bursts are in flight (acceptance 8). */
	Lifetime
};

/** One scenario step; only the fields named in each comment are read for that step type. */
USTRUCT(BlueprintType)
struct FSTSScenarioStep
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS")
	ESTSScenarioStepType Type = ESTSScenarioStepType::Wait;

	/** Wait: seconds. WaitForIdle: timeout. RecordStart: duration (0 = until RecordStop). SpreadStart, BurstRepeat: duration (0 = until stopped). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS", meta = (ClampMin = "0.0"))
	float DurationSec = 0.0f;

	/** RecordStart: the recording name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS")
	FString Name;

	/** SpreadStart: requests per second. BurstRepeat: bursts per second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS", meta = (ClampMin = "0.0"))
	float Rate = 5.0f;

	/** SetSeed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS")
	int32 Seed = 12345;

	/** SetPattern. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS")
	ESTSUpdatePattern Pattern = ESTSUpdatePattern::StepUp;

	/** SetPattern: weights used by Mixed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS", meta = (ClampMin = "0.0"))
	FVector MixedWeights = FVector(1.0, 1.0, 1.0);

	/** SetTargetMode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS")
	ESTSTargetMode TargetMode = ESTSTargetMode::Random;

	/** SetTargetMode (RankRange): inclusive distance-rank range from the grid centre. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS")
	FIntPoint RankRange = FIntPoint(0, 999);

	/** SetTargetMode (Patch): centre cell, size in cells, and the movement in cells per trigger. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS")
	FIntPoint PatchCenter = FIntPoint(12, 10);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS")
	FIntPoint PatchSize = FIntPoint(6, 6);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS")
	FIntPoint PatchStep = FIntPoint(0, 0);

	/** Burst, BurstRepeat: counts by requested (target) resolution. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS", meta = (ClampMin = "0"))
	int32 Burst4k = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS", meta = (ClampMin = "0"))
	int32 Burst2k = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS", meta = (ClampMin = "0"))
	int32 Burst1k = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "STS", meta = (ClampMin = "0"))
	int32 Burst512 = 0;
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

	template <typename EnumType>
	FString EnumToString(EnumType Value)
	{
		const UEnum* Enum = StaticEnum<EnumType>();
		return Enum ? Enum->GetNameStringByValue(int64(Value)) : FString();
	}
}

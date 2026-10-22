// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "STSTypes.h"

/** Look settings for one resolution; BaseColor already carries the per-resolution colour. */
struct FSTSTextureLook
{
	FColor BaseColor = FColor::White;
	FColor BorderColor = FColor(8, 8, 8);
	float BorderWidthFraction = 0.004f;
	bool bDrawDiagonal = true;
	ESTSMipCoding MipCoding = ESTSMipCoding::Brightness;
	float MipBrightnessLow = 0.55f;

	bool operator==(const FSTSTextureLook& Other) const
	{
		return BaseColor == Other.BaseColor
			&& BorderColor == Other.BorderColor
			&& BorderWidthFraction == Other.BorderWidthFraction
			&& bDrawDiagonal == Other.bDrawDiagonal
			&& MipCoding == Other.MipCoding
			&& MipBrightnessLow == Other.MipBrightnessLow;
	}
};

/** A full DXT1 mip chain, base level first. */
struct FSTSMipChain
{
	int32 Width = 0;
	int32 Height = 0;
	TArray<TArray<uint8>> Mips;
	int64 TotalBytes = 0;
};

class FSTSDxt1Generator
{
public:
	static int32 GetMipCount(int32 Width, int32 Height);

	/** DXT1 mip size: max(1, w/4) * max(1, h/4) * 8 bytes. */
	static int64 GetMipBytes(int32 Width, int32 Height);

	static int64 GetChainBytes(int32 Width, int32 Height);

	/**
	 * Authors every mip independently (no downsampling) and encodes it as DXT1 in 4-colour mode.
	 * Variant 0 is the plain Phase 0 look. Variant k > 0 adds k bars of the inverted base colour along u - v = const,
	 * crossing both triangle orientations and kept at least one texel wide down to the 1x1 mip.
	 */
	static void GenerateChain(int32 Width, int32 Height, const FSTSTextureLook& Look, uint32 Variant, FSTSMipChain& OutChain);
};

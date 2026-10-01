// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSDxt1Generator.h"

#include "Async/ParallelFor.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
	struct FRgb
	{
		int32 R = 0;
		int32 G = 0;
		int32 B = 0;

		bool operator==(const FRgb& Other) const
		{
			return R == Other.R && G == Other.G && B == Other.B;
		}
	};

	FORCEINLINE FRgb ToRgb(const FColor& Color, float Scale)
	{
		return { FMath::Clamp(FMath::RoundToInt(Color.R * Scale), 0, 255), FMath::Clamp(FMath::RoundToInt(Color.G * Scale), 0, 255), FMath::Clamp(FMath::RoundToInt(Color.B * Scale), 0, 255) };
	}

	FORCEINLINE uint16 ToRgb565(const FRgb& Color)
	{
		return uint16((((Color.R * 31 + 127) / 255) << 11) | (((Color.G * 63 + 127) / 255) << 5) | ((Color.B * 31 + 127) / 255));
	}

	FORCEINLINE FRgb FromRgb565(uint16 Value)
	{
		const int32 R5 = (Value >> 11) & 31;
		const int32 G6 = (Value >> 5) & 63;
		const int32 B5 = Value & 31;
		return { (R5 << 3) | (R5 >> 2), (G6 << 2) | (G6 >> 4), (B5 << 3) | (B5 >> 2) };
	}

	FORCEINLINE int32 DistanceSq(const FRgb& A, const FRgb& B)
	{
		const int32 DR = A.R - B.R;
		const int32 DG = A.G - B.G;
		const int32 DB = A.B - B.B;
		return DR * DR + DG * DG + DB * DB;
	}

	struct FMipContext
	{
		int32 Width = 0;
		int32 Height = 0;
		int32 BorderTexels = 1;
		bool bDrawLines = false;
		bool bDrawDiagonal = false;
		int32 CheckerCell = 0;
		FRgb Base;
		FRgb BaseDark;
		FRgb Border;
	};

	FMipContext MakeMipContext(const FSTSTextureLook& Look, int32 MipIndex, int32 Width, int32 Height)
	{
		FMipContext Context;
		Context.Width = Width;
		Context.Height = Height;
		Context.BorderTexels = FMath::Max(1, FMath::RoundToInt(Width * Look.BorderWidthFraction));

		// Below 4 texels a border would swallow the whole mip and hide the mip coding.
		Context.bDrawLines = FMath::Min(Width, Height) >= 4;
		Context.bDrawDiagonal = Look.bDrawDiagonal;
		Context.Border = ToRgb(Look.BorderColor, 1.0f);

		float BaseScale = 1.0f;
		if (Look.MipCoding == ESTSMipCoding::Brightness && (MipIndex & 1))
		{
			BaseScale = Look.MipBrightnessLow;
		}
		Context.Base = ToRgb(Look.BaseColor, BaseScale);

		Context.BaseDark = Context.Base;
		if (Look.MipCoding == ESTSMipCoding::Checker)
		{
			// Cells across the mip cycle through 2, 4, 8 and 16, so every level has a distinct frequency.
			const int32 CellsAcross = 2 << (MipIndex & 3);
			Context.CheckerCell = FMath::Max(1, Width / CellsAcross);
			Context.BaseDark = ToRgb(Look.BaseColor, Look.MipBrightnessLow);
		}
		return Context;
	}

	FRgb ShadeTexel(const FMipContext& Context, int32 X, int32 Y)
	{
		if (Context.bDrawLines)
		{
			const int32 B = Context.BorderTexels;
			if (X < B || Y < B || X >= Context.Width - B || Y >= Context.Height - B)
			{
				return Context.Border;
			}

			if (Context.bDrawDiagonal)
			{
				const float U = (X + 0.5f) / Context.Width;
				const float V = (Y + 0.5f) / Context.Height;
				// Perpendicular distance to u + v = 1, in texels; each triangle sees one side of the band.
				if (FMath::Abs(U + V - 1.0f) * Context.Width * 0.70710678f <= B)
				{
					return Context.Border;
				}
			}
		}

		if (Context.CheckerCell > 0 && (((X / Context.CheckerCell) + (Y / Context.CheckerCell)) & 1))
		{
			return Context.BaseDark;
		}
		return Context.Base;
	}

	void EncodeBlock(const FRgb (&Texels)[16], uint8* OutBlock)
	{
		FRgb Unique[16];
		int32 NumUnique = 0;
		for (const FRgb& Texel : Texels)
		{
			bool bFound = false;
			for (int32 Index = 0; Index < NumUnique; ++Index)
			{
				if (Unique[Index] == Texel)
				{
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				Unique[NumUnique++] = Texel;
			}
		}

		uint16 Color0 = ToRgb565(Unique[0]);
		uint16 Color1 = Color0;
		if (NumUnique > 1)
		{
			int32 BestA = 0;
			int32 BestB = 1;
			int32 BestDistance = -1;
			for (int32 A = 0; A < NumUnique; ++A)
			{
				for (int32 B = A + 1; B < NumUnique; ++B)
				{
					const int32 Distance = DistanceSq(Unique[A], Unique[B]);
					if (Distance > BestDistance)
					{
						BestDistance = Distance;
						BestA = A;
						BestB = B;
					}
				}
			}
			Color0 = ToRgb565(Unique[BestA]);
			Color1 = ToRgb565(Unique[BestB]);
		}

		// color0 > color1 selects the 4-colour mode; equal endpoints would select the 3-colour + transparent mode.
		if (Color0 < Color1)
		{
			Swap(Color0, Color1);
		}
		if (Color0 == Color1)
		{
			if (Color1 > 0)
			{
				--Color1;
			}
			else
			{
				Color0 = 1;
			}
		}

		uint32 Indices = 0;
		if (NumUnique > 1)
		{
			const FRgb P0 = FromRgb565(Color0);
			const FRgb P1 = FromRgb565(Color1);
			const FRgb Palette[4] =
			{
				P0,
				P1,
				{ (2 * P0.R + P1.R) / 3, (2 * P0.G + P1.G) / 3, (2 * P0.B + P1.B) / 3 },
				{ (P0.R + 2 * P1.R) / 3, (P0.G + 2 * P1.G) / 3, (P0.B + 2 * P1.B) / 3 }
			};

			for (int32 TexelIndex = 0; TexelIndex < 16; ++TexelIndex)
			{
				int32 BestIndex = 0;
				int32 BestDistance = DistanceSq(Texels[TexelIndex], Palette[0]);
				for (int32 PaletteIndex = 1; PaletteIndex < 4; ++PaletteIndex)
				{
					const int32 Distance = DistanceSq(Texels[TexelIndex], Palette[PaletteIndex]);
					if (Distance < BestDistance)
					{
						BestDistance = Distance;
						BestIndex = PaletteIndex;
					}
				}
				Indices |= uint32(BestIndex) << (2 * TexelIndex);
			}
		}

		OutBlock[0] = uint8(Color0 & 0xFF);
		OutBlock[1] = uint8(Color0 >> 8);
		OutBlock[2] = uint8(Color1 & 0xFF);
		OutBlock[3] = uint8(Color1 >> 8);
		OutBlock[4] = uint8(Indices & 0xFF);
		OutBlock[5] = uint8((Indices >> 8) & 0xFF);
		OutBlock[6] = uint8((Indices >> 16) & 0xFF);
		OutBlock[7] = uint8((Indices >> 24) & 0xFF);
	}

	void GenerateMip(const FSTSTextureLook& Look, int32 MipIndex, int32 Width, int32 Height, TArray<uint8>& OutBytes)
	{
		const int32 BlocksX = FMath::Max(1, Width / 4);
		const int32 BlocksY = FMath::Max(1, Height / 4);
		OutBytes.SetNumUninitialized(BlocksX * BlocksY * 8);

		const FMipContext Context = MakeMipContext(Look, MipIndex, Width, Height);
		uint8* Output = OutBytes.GetData();

		ParallelFor(BlocksY, [&Context, Output, BlocksX, Width, Height](int32 BlockY)
		{
			for (int32 BlockX = 0; BlockX < BlocksX; ++BlockX)
			{
				FRgb Texels[16];
				for (int32 TexelY = 0; TexelY < 4; ++TexelY)
				{
					for (int32 TexelX = 0; TexelX < 4; ++TexelX)
					{
						// Mips smaller than one block replicate their edge texels into the padding.
						const int32 X = FMath::Min(BlockX * 4 + TexelX, Width - 1);
						const int32 Y = FMath::Min(BlockY * 4 + TexelY, Height - 1);
						Texels[TexelY * 4 + TexelX] = ShadeTexel(Context, X, Y);
					}
				}
				EncodeBlock(Texels, Output + (int64(BlockY) * BlocksX + BlockX) * 8);
			}
		});
	}
}

int32 FSTSDxt1Generator::GetMipCount(int32 Width, int32 Height)
{
	return int32(FMath::FloorLog2(uint32(FMath::Max(Width, Height)))) + 1;
}

int64 FSTSDxt1Generator::GetMipBytes(int32 Width, int32 Height)
{
	return int64(FMath::Max(1, Width / 4)) * FMath::Max(1, Height / 4) * 8;
}

int64 FSTSDxt1Generator::GetChainBytes(int32 Width, int32 Height)
{
	int64 Total = 0;
	const int32 MipCount = GetMipCount(Width, Height);
	for (int32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
	{
		Total += GetMipBytes(FMath::Max(1, Width >> MipIndex), FMath::Max(1, Height >> MipIndex));
	}
	return Total;
}

void FSTSDxt1Generator::GenerateChain(int32 Width, int32 Height, const FSTSTextureLook& Look, uint32 /*Variant*/, FSTSMipChain& OutChain)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(STS_GenerateChain);

	const int32 MipCount = GetMipCount(Width, Height);
	OutChain.Width = Width;
	OutChain.Height = Height;
	OutChain.TotalBytes = 0;
	OutChain.Mips.Reset(MipCount);
	OutChain.Mips.SetNum(MipCount);

	for (int32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
	{
		GenerateMip(Look, MipIndex, FMath::Max(1, Width >> MipIndex), FMath::Max(1, Height >> MipIndex), OutChain.Mips[MipIndex]);
		OutChain.TotalBytes += OutChain.Mips[MipIndex].Num();
	}
}

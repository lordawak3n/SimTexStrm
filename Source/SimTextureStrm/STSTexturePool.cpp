// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSTexturePool.h"

#include "TextureResource.h"

UTexture2D* FSTSTexturePool::CreateSpareTexture(int32 Resolution, bool bSRGB)
{
	UTexture2D* Texture = UTexture2D::CreateTransient(Resolution, Resolution, PF_DXT1);
	if (!Texture)
	{
		return nullptr;
	}

	// CreateTransient allocates the base mip; a spare has none until its first use.
	Texture->GetPlatformData()->Mips.Empty();
	Texture->SRGB = bSRGB;
	Texture->LODGroup = TEXTUREGROUP_World;
	Texture->NeverStream = true;
	return Texture;
}

void FSTSTexturePool::Reset()
{
	Members.Empty();
	for (int32 Bucket = 0; Bucket < STS::NumBuckets; ++Bucket)
	{
		Free[Bucket].Empty();
		InUse[Bucket] = 0;
		Spares[Bucket] = 0;
	}
}

void FSTSTexturePool::AddInUse(UTexture2D* Texture, int32 Bucket)
{
	Members.Add(Texture);
	++InUse[Bucket];
}

void FSTSTexturePool::AddSpare(UTexture2D* Texture, int32 Bucket)
{
	Members.Add(Texture);
	++Spares[Bucket];
	Free[Bucket].EmplaceLast(FEntry{ Texture, NeverReleased });
}

UTexture2D* FSTSTexturePool::TakeBottom(int32 Bucket, uint64& OutReleaseFrame)
{
	TDeque<FEntry>& Queue = Free[Bucket];
	if (Queue.IsEmpty())
	{
		return nullptr;
	}

	const FEntry Entry = Queue.First();
	Queue.PopFirst();
	++InUse[Bucket];
	OutReleaseFrame = Entry.ReleaseFrame;
	return Entry.Texture;
}

void FSTSTexturePool::Release(UTexture2D* Texture, int32 Bucket, uint64 Frame)
{
	--InUse[Bucket];
	Free[Bucket].EmplaceLast(FEntry{ Texture, Frame });
}

void FSTSTexturePool::ComputeRetainedBytes(int64 (&OutBytes)[STS::NumBuckets]) const
{
	for (int64& Bytes : OutBytes)
	{
		Bytes = 0;
	}
	for (const TObjectPtr<UTexture2D>& Member : Members)
	{
		const FTexturePlatformData* PlatformData = Member ? Member->GetPlatformData() : nullptr;
		if (!PlatformData)
		{
			continue;
		}
		const int32 Bucket = STS::ResolutionToBucket(PlatformData->SizeX);
		if (Bucket == INDEX_NONE)
		{
			continue;
		}
		for (const FTexture2DMipMap& Mip : PlatformData->Mips)
		{
			if (Mip.BulkData.IsBulkDataLoaded())
			{
				OutBytes[Bucket] += Mip.BulkData.GetBulkDataSize();
			}
		}
	}
}

void FSTSTexturePool::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObjects(Members);
}

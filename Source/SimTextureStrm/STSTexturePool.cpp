// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSTexturePool.h"

#include "STSDxt1Generator.h"
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
		RetainedBytes[Bucket] = 0;
	}
}

void FSTSTexturePool::AddInUse(UTexture2D* Texture, int32 Bucket, bool bHasMips)
{
	Members.Add(Texture);
	++InUse[Bucket];
	if (bHasMips)
	{
		RetainedBytes[Bucket] += FSTSDxt1Generator::GetChainBytes(STS::BucketResolutions[Bucket], STS::BucketResolutions[Bucket]);
	}
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

void FSTSTexturePool::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObjects(Members);
}

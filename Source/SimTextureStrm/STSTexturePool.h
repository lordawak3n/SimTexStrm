// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Deque.h"
#include "Engine/Texture2D.h"
#include "STSTypes.h"
#include "UObject/GCObject.h"

/**
 * The production texture pools: one circular FIFO queue of free UTexture2D objects per resolution.
 * Released objects go on the top (the back); objects are taken from the bottom (the front).
 * Keeps every member alive, whether it is on a tile or in a queue. Game thread only.
 */
class FSTSTexturePool final : public FGCObject
{
public:
	/** Release frame of an object that has never been released (a fresh spare). */
	static constexpr uint64 NeverReleased = MAX_uint64;

	FSTSTexturePool() = default;

	UE_NONCOPYABLE(FSTSTexturePool);

	/** Creates a UTexture2D with the right size and format, no mips and no GPU resource; both come on first use. */
	static UTexture2D* CreateSpareTexture(int32 Resolution, bool bSRGB);

	void Reset();

	/** Registers a tile's current texture. bHasMips is whether its PlatformData holds a mip chain. */
	void AddInUse(UTexture2D* Texture, int32 Bucket, bool bHasMips);

	/** Registers a spare and puts it on the top of its queue. */
	void AddSpare(UTexture2D* Texture, int32 Bucket);

	/** Takes the bottom object, or null if the queue is empty. OutReleaseFrame is NeverReleased for a fresh spare. */
	UTexture2D* TakeBottom(int32 Bucket, uint64& OutReleaseFrame);

	/** Puts a texture that left a tile on the top of its queue. */
	void Release(UTexture2D* Texture, int32 Bucket, uint64 Frame);

	void AddRetainedBytes(int32 Bucket, int64 Delta) { RetainedBytes[Bucket] += Delta; }

	int32 GetNumFree(int32 Bucket) const { return Free[Bucket].Num(); }
	int32 GetNumInUse(int32 Bucket) const { return InUse[Bucket]; }
	int32 GetNumSpares(int32 Bucket) const { return Spares[Bucket]; }

	/** CPU memory held by the mips in the PlatformData of every member of the bucket, free or in use. */
	int64 GetRetainedBytes(int32 Bucket) const { return RetainedBytes[Bucket]; }

	//~ Begin FGCObject Interface
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FSTSTexturePool"); }
	//~ End FGCObject Interface

private:
	struct FEntry
	{
		UTexture2D* Texture = nullptr;
		uint64 ReleaseFrame = NeverReleased;
	};

	TArray<TObjectPtr<UTexture2D>> Members;
	TDeque<FEntry> Free[STS::NumBuckets];
	int32 InUse[STS::NumBuckets] = {};
	int32 Spares[STS::NumBuckets] = {};
	int64 RetainedBytes[STS::NumBuckets] = {};
};

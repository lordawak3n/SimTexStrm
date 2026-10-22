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

	/** Registers a tile's current texture. */
	void AddInUse(UTexture2D* Texture, int32 Bucket);

	/** Registers a spare and puts it on the top of its queue. */
	void AddSpare(UTexture2D* Texture, int32 Bucket);

	/** Takes the bottom object, or null if the queue is empty. OutReleaseFrame is NeverReleased for a fresh spare. */
	UTexture2D* TakeBottom(int32 Bucket, uint64& OutReleaseFrame);

	/** Puts a texture that left a tile on the top of its queue. */
	void Release(UTexture2D* Texture, int32 Bucket, uint64 Frame);

	int32 GetNumFree(int32 Bucket) const { return Free[Bucket].Num(); }
	int32 GetNumInUse(int32 Bucket) const { return InUse[Bucket]; }
	int32 GetNumSpares(int32 Bucket) const { return Spares[Bucket]; }
	int32 GetNumMembers(int32 Bucket) const { return InUse[Bucket] + Free[Bucket].Num(); }

	/**
	 * CPU memory held by the mip bulk data of every member of each bucket, free or in use: the size of every mip whose bulk data is still loaded.
	 * Walks the members, so it reflects BULKDATA_SingleUse consumption (a consumed mip keeps its size but no longer holds memory).
	 */
	void ComputeRetainedBytes(int64 (&OutBytes)[STS::NumBuckets]) const;

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
};

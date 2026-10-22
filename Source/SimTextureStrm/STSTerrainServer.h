// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/MpscQueue.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "STSDxt1Generator.h"

/** The client-side buffer a response is copied into. Swappable for a pooled buffer in a later phase. */
using FSTSPayloadBuffer = TArray<uint8>;

struct FSTSMipRange
{
	int64 Offset = 0;
	int64 Size = 0;
};

/** One library entry: all mips of one (resolution, variant) in a single contiguous DXT1 buffer, base level first. */
struct FSTSPayload
{
	int32 Resolution = 0;
	int32 Variant = 0;
	FSTSPayloadBuffer Data;
	TArray<FSTSMipRange> Mips;
	uint64 Checksum = 0;

	static TSharedRef<const FSTSPayload> FromChain(int32 Resolution, int32 Variant, const FSTSMipChain& Chain);

	/** CityHash64 (Hash/CityHash.h). */
	static uint64 ComputeChecksum(const uint8* Bytes, int64 NumBytes);
};

/** Immutable once built, and entries are shared with in-flight jobs, so rebuilding it never invalidates a request. */
class FSTSPayloadLibrary
{
public:
	void Add(const TSharedRef<const FSTSPayload>& Payload);
	TSharedPtr<const FSTSPayload> Find(int32 Resolution, int32 Variant) const;
	int32 Num() const { return Entries.Num(); }
	int64 GetTotalBytes() const { return TotalBytes; }

private:
	TMap<int64, TSharedRef<const FSTSPayload>> Entries;
	int64 TotalBytes = 0;
};

struct FSTSRequest
{
	int64 RequestId = INDEX_NONE;
	int32 TileIndex = INDEX_NONE;
	uint32 TileEpoch = 0;
	int32 Resolution = 0;
	int32 Variant = 0;
	uint64 IssueFrame = 0;
	double IssueTimeSec = 0.0;
	/** The tile's distance rank (lower is nearer); unused until a later phase budgets uploads. */
	int32 Priority = 0;
};

struct FSTSResponse
{
	FSTSRequest Request;
	/** Gives the mip offsets and sizes of Buffer. */
	TSharedPtr<const FSTSPayload> Source;
	FSTSPayloadBuffer Buffer;
	uint64 Checksum = 0;
	double ReadyTimeSec = 0.0;
	double WorkerDoneTimeSec = 0.0;
	/** The worker finished after ReadyTimeSec, so the model was not met. */
	bool bLate = false;
	/** The worker skipped the copy because the epoch was already stale. */
	bool bCopySkipped = false;
};

struct FSTSServerSettings
{
	float LatencyMs = 20.0f;
	float LatencyJitterMs = 5.0f;
	/** In MB (1048576 bytes) per second; 0 is unlimited. */
	float LinkBandwidthMBps = 0.0f;
	/** Requests admitted at once; the rest wait in a game-thread backlog. 0 is unlimited. */
	int32 MaxInFlight = 0;
	/** Debug: workers flip one byte of every Nth response (by request ID) to exercise the checksum path. 0 is off. */
	int32 CorruptEveryN = 0;
};

struct FSTSServerCounters
{
	int64 Submitted = 0;
	int64 Released = 0;
	int64 StaleDiscarded = 0;
	int64 Late = 0;
	int64 BytesReleased = 0;
};

/**
 * Synthetic terrain server. Workers (dedicated FRunnableThreads) only memcpy library entries into new buffers.
 * Delivery timing is a model computed on the game thread, so it is deterministic and nothing sleeps.
 * Every method except the worker loop is game-thread only.
 */
class FSTSTerrainServer
{
public:
	explicit FSTSTerrainServer(int32 InWorkerCount);
	~FSTSTerrainServer();

	UE_NONCOPYABLE(FSTSTerrainServer);

	void SetSettings(const FSTSServerSettings& InSettings) { Settings = InSettings; }

	/** Responses of any other epoch are discarded and counted; workers skip their copy. */
	void SetEpoch(uint32 NewEpoch);

	/** Models the response time and queues the copy. Requests over MaxInFlight wait in a backlog until a slot frees. */
	void Submit(const FSTSRequest& Request, const TSharedRef<const FSTSPayload>& Payload, double NowSec);

	/** Drains finished copies into the delay line, drops stale ones, and appends the responses due at NowSec in ready order. */
	void Pump(double NowSec, TArray<FSTSResponse>& OutReleased);

	/** Stops the workers and frees everything still queued, in flight or in the delay line. */
	void Shutdown();

	int32 GetWorkerCount() const { return WorkerCount; }

	/** Requests submitted and not yet released or discarded, including the backlog and the delay line. */
	int32 GetInFlight() const { return InFlight; }

	const FSTSServerCounters& GetCounters() const { return Counters; }

private:
	class FWorker;

	struct FJob
	{
		FSTSRequest Request;
		TSharedPtr<const FSTSPayload> Payload;
		double ReadyTimeSec = 0.0;
		bool bCorrupt = false;
	};

	void Admit(const FJob& Job, double NowSec);
	void AdmitBacklog(double NowSec);
	void Discard(FSTSResponse& Response);
	bool TryTakeJob(FJob& OutJob);
	void ProcessJob(FJob& Job);

	FSTSServerSettings Settings;
	int32 WorkerCount = 0;
	std::atomic<uint32> Epoch{ 0 };
	std::atomic<bool> bStopping{ false };

	FCriticalSection JobLock;
	TQueue<FJob> Jobs;
	TMpscQueue<FSTSResponse> Completed;
	TArray<TUniquePtr<FWorker>> Workers;

	TQueue<FJob> Backlog;
	TArray<FSTSResponse> DelayLine;
	double LinkFreeAtSec = 0.0;
	int32 InFlight = 0;
	int32 Admitted = 0;
	FSTSServerCounters Counters;
};

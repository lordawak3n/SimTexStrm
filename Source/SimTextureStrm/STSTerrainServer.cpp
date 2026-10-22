// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSTerrainServer.h"

#include "Algo/Sort.h"
#include "HAL/Event.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Hash/CityHash.h"
#include "Math/RandomStream.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "STSTextureUploader.h"

namespace
{
	int64 MakeLibraryKey(int32 Resolution, int32 Variant)
	{
		return (int64(Resolution) << 32) | int64(uint32(Variant));
	}
}

// --- Payloads ---

uint64 FSTSPayload::ComputeChecksum(const uint8* Bytes, int64 NumBytes)
{
	check(NumBytes >= 0 && NumBytes <= int64(MAX_uint32));
	return CityHash64(reinterpret_cast<const char*>(Bytes), uint32(NumBytes));
}

TSharedRef<const FSTSPayload> FSTSPayload::FromChain(int32 Resolution, int32 Variant, const FSTSMipChain& Chain)
{
	TSharedRef<FSTSPayload> Payload = MakeShared<FSTSPayload>();
	Payload->Resolution = Resolution;
	Payload->Variant = Variant;
	Payload->Data.SetNumUninitialized(int32(Chain.TotalBytes));
	Payload->Mips.Reserve(Chain.Mips.Num());

	int64 Offset = 0;
	for (const TArray<uint8>& Mip : Chain.Mips)
	{
		Payload->Mips.Add({ Offset, Mip.Num() });
		FMemory::Memcpy(Payload->Data.GetData() + Offset, Mip.GetData(), Mip.Num());
		Offset += Mip.Num();
	}
	Payload->Checksum = ComputeChecksum(Payload->Data.GetData(), Payload->Data.Num());
	return Payload;
}

void FSTSPayloadLibrary::Add(const TSharedRef<const FSTSPayload>& Payload)
{
	if (const TSharedRef<const FSTSPayload>* Existing = Entries.Find(MakeLibraryKey(Payload->Resolution, Payload->Variant)))
	{
		TotalBytes -= (*Existing)->Data.Num();
	}
	Entries.Add(MakeLibraryKey(Payload->Resolution, Payload->Variant), Payload);
	TotalBytes += Payload->Data.Num();
}

TSharedPtr<const FSTSPayload> FSTSPayloadLibrary::Find(int32 Resolution, int32 Variant) const
{
	const TSharedRef<const FSTSPayload>* Found = Entries.Find(MakeLibraryKey(Resolution, Variant));
	return Found ? TSharedPtr<const FSTSPayload>(*Found) : nullptr;
}

// --- Server ---

class FSTSTerrainServer::FWorker final : public FRunnable
{
public:
	FWorker(FSTSTerrainServer& InServer, int32 Index)
		: Server(InServer)
	{
		Thread = FRunnableThread::Create(this, *FString::Printf(TEXT("STSTerrainServer %d"), Index), 0, TPri_Normal);
	}

	virtual ~FWorker() override
	{
		if (Thread)
		{
			Thread->Kill(true);
			delete Thread;
		}
	}

	void Wake()
	{
		WakeEvent->Trigger();
	}

	//~ Begin FRunnable Interface
	virtual uint32 Run() override
	{
		FJob Job;
		while (!Server.bStopping.load(std::memory_order_acquire))
		{
			while (!Server.bStopping.load(std::memory_order_acquire) && Server.TryTakeJob(Job))
			{
				Server.ProcessJob(Job);
				Job = FJob();
			}
			WakeEvent->Wait();
		}
		return 0;
	}

	virtual void Stop() override
	{
		WakeEvent->Trigger();
	}
	//~ End FRunnable Interface

private:
	FSTSTerrainServer& Server;
	FEventRef WakeEvent;
	FRunnableThread* Thread = nullptr;
};

FSTSTerrainServer::FSTSTerrainServer(int32 InWorkerCount)
	: WorkerCount(FMath::Clamp(InWorkerCount, 1, 32))
{
	Workers.Reserve(WorkerCount);
	for (int32 Index = 0; Index < WorkerCount; ++Index)
	{
		Workers.Add(MakeUnique<FWorker>(*this, Index));
	}
}

FSTSTerrainServer::~FSTSTerrainServer()
{
	Shutdown();
}

void FSTSTerrainServer::Shutdown()
{
	bStopping.store(true, std::memory_order_release);
	for (const TUniquePtr<FWorker>& Worker : Workers)
	{
		Worker->Wake();
	}
	Workers.Reset();

	FSTSResponse Response;
	while (Completed.Dequeue(Response))
	{
		Response = FSTSResponse();
	}
	{
		FScopeLock Lock(&JobLock);
		Jobs.Empty();
	}
	Backlog.Empty();
	DelayLine.Empty();
	InFlight = 0;
	Admitted = 0;
}

void FSTSTerrainServer::SetEpoch(uint32 NewEpoch)
{
	Epoch.store(NewEpoch, std::memory_order_release);
}

void FSTSTerrainServer::SetBufferPool(const TSharedPtr<FSTSPayloadBufferPool, ESPMode::ThreadSafe>& InPool)
{
	FScopeLock Lock(&BufferPoolLock);
	BufferPool = InPool;
}

TSharedPtr<FSTSPayloadBufferPool, ESPMode::ThreadSafe> FSTSTerrainServer::GetBufferPool() const
{
	FScopeLock Lock(&BufferPoolLock);
	return BufferPool;
}

void FSTSTerrainServer::Submit(const FSTSRequest& Request, const TSharedRef<const FSTSPayload>& Payload, double NowSec)
{
	if (bStopping.load(std::memory_order_acquire))
	{
		return;
	}

	++InFlight;
	++Counters.Submitted;

	FJob Job;
	Job.Request = Request;
	Job.Payload = Payload;
	Job.bCorrupt = Settings.CorruptEveryN > 0 && (Request.RequestId % Settings.CorruptEveryN) == Settings.CorruptEveryN - 1;

	if (!Backlog.IsEmpty() || (Settings.MaxInFlight > 0 && Admitted >= Settings.MaxInFlight))
	{
		Backlog.Enqueue(MoveTemp(Job));
		return;
	}
	Admit(Job, NowSec);
}

void FSTSTerrainServer::Admit(const FJob& Job, double NowSec)
{
	FJob Modelled = Job;

	// The jitter is seeded by the request ID alone, so it repeats exactly across runs.
	FRandomStream JitterStream(int32(HashCombineFast(GetTypeHash(Job.Request.RequestId), 0x9E3779B9u)));
	const double JitterMs = Settings.LatencyJitterMs > 0.0f ? JitterStream.FRandRange(-Settings.LatencyJitterMs, Settings.LatencyJitterMs) : 0.0;
	const double LatencySec = FMath::Max(0.0, Settings.LatencyMs + JitterMs) * 0.001;
	const double TransferSec = Settings.LinkBandwidthMBps > 0.0f ? double(Job.Payload->Data.Num()) / (double(Settings.LinkBandwidthMBps) * STS::MegaByte) : 0.0;

	Modelled.ReadyTimeSec = FMath::Max(NowSec + LatencySec, LinkFreeAtSec) + TransferSec;
	LinkFreeAtSec = Modelled.ReadyTimeSec;
	++Admitted;

	{
		FScopeLock Lock(&JobLock);
		Jobs.Enqueue(MoveTemp(Modelled));
	}
	for (const TUniquePtr<FWorker>& Worker : Workers)
	{
		Worker->Wake();
	}
}

void FSTSTerrainServer::AdmitBacklog(double NowSec)
{
	FJob Job;
	while (!Backlog.IsEmpty() && (Settings.MaxInFlight <= 0 || Admitted < Settings.MaxInFlight))
	{
		Backlog.Dequeue(Job);
		if (Job.Request.TileEpoch != Epoch.load(std::memory_order_acquire))
		{
			--InFlight;
			++Counters.StaleDiscarded;
			continue;
		}
		Admit(Job, NowSec);
	}
}

bool FSTSTerrainServer::TryTakeJob(FJob& OutJob)
{
	FScopeLock Lock(&JobLock);
	return Jobs.Dequeue(OutJob);
}

void FSTSTerrainServer::ProcessJob(FJob& Job)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(STS_ServerWorkerCopy);

	FSTSResponse Response;
	Response.Request = Job.Request;
	Response.Source = Job.Payload;
	Response.Checksum = Job.Payload->Checksum;
	Response.ReadyTimeSec = Job.ReadyTimeSec;

	if (Job.Request.TileEpoch != Epoch.load(std::memory_order_acquire))
	{
		Response.bCopySkipped = true;
	}
	else
	{
		const FSTSPayloadBuffer& Source = Job.Payload->Data;
		const TSharedPtr<FSTSPayloadBufferPool, ESPMode::ThreadSafe> Pool = GetBufferPool();
		const int32 Bucket = STS::ResolutionToBucket(Job.Request.Resolution);
		if (Pool.IsValid() && Bucket != INDEX_NONE)
		{
			Response.Buffer = Pool->Acquire(Bucket, Source.Num());
		}
		else
		{
			Response.Buffer.SetNumUninitialized(Source.Num());
		}
		FMemory::Memcpy(Response.Buffer.GetData(), Source.GetData(), Source.Num());
		if (Job.bCorrupt && Source.Num() > 0)
		{
			Response.Buffer[Source.Num() / 2] ^= 0xFF;
		}
	}

	Response.WorkerDoneTimeSec = FPlatformTime::Seconds();
	Completed.Enqueue(MoveTemp(Response));
}

void FSTSTerrainServer::Discard(FSTSResponse& Response)
{
	--InFlight;
	--Admitted;
	++Counters.StaleDiscarded;

	const TSharedPtr<FSTSPayloadBufferPool, ESPMode::ThreadSafe> Pool = GetBufferPool();
	const int32 Bucket = STS::ResolutionToBucket(Response.Request.Resolution);
	if (Pool.IsValid() && Bucket != INDEX_NONE)
	{
		Pool->Release(Bucket, MoveTemp(Response.Buffer));
	}
	Response.Buffer.Empty();
}

void FSTSTerrainServer::Pump(double NowSec, TArray<FSTSResponse>& OutReleased)
{
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(STS_ServerDrain);

		const uint32 CurrentEpoch = Epoch.load(std::memory_order_acquire);
		FSTSResponse Response;
		while (Completed.Dequeue(Response))
		{
			if (Response.Request.TileEpoch != CurrentEpoch)
			{
				Discard(Response);
				continue;
			}
			Response.bLate = Response.WorkerDoneTimeSec > Response.ReadyTimeSec;
			DelayLine.Add(MoveTemp(Response));
		}

		const int32 FirstReleased = OutReleased.Num();
		int32 Kept = 0;
		for (int32 Index = 0; Index < DelayLine.Num(); ++Index)
		{
			FSTSResponse& Candidate = DelayLine[Index];
			if (Candidate.Request.TileEpoch != CurrentEpoch)
			{
				Discard(Candidate);
				continue;
			}
			if (NowSec >= Candidate.ReadyTimeSec)
			{
				--InFlight;
				--Admitted;
				++Counters.Released;
				Counters.Late += Candidate.bLate ? 1 : 0;
				Counters.BytesReleased += Candidate.Buffer.Num();
				OutReleased.Add(MoveTemp(Candidate));
				continue;
			}
			if (Kept != Index)
			{
				DelayLine[Kept] = MoveTemp(Candidate);
			}
			++Kept;
		}
		DelayLine.SetNum(Kept, EAllowShrinking::No);

		TArrayView<FSTSResponse> Released(OutReleased.GetData() + FirstReleased, OutReleased.Num() - FirstReleased);
		Algo::Sort(Released, [](const FSTSResponse& A, const FSTSResponse& B)
		{
			return A.ReadyTimeSec < B.ReadyTimeSec || (A.ReadyTimeSec == B.ReadyTimeSec && A.Request.RequestId < B.Request.RequestId);
		});
	}

	AdmitBacklog(NowSec);
}

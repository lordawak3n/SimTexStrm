// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSMipBuilder.h"

#include "HAL/Event.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

class FSTSMipBuilder::FWorker final : public FRunnable
{
public:
	FWorker(FSTSMipBuilder& InBuilder, int32 Index)
		: Builder(InBuilder)
	{
		Thread = FRunnableThread::Create(this, *FString::Printf(TEXT("STSMipBuilder %d"), Index), 0, TPri_Normal);
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
		FSTSBuildJob Job;
		while (!Builder.bStopping.load(std::memory_order_acquire))
		{
			while (!Builder.bStopping.load(std::memory_order_acquire) && Builder.TryTakeJob(Job))
			{
				Builder.Process(Job);
				Job = FSTSBuildJob();
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
	FSTSMipBuilder& Builder;
	FEventRef WakeEvent;
	FRunnableThread* Thread = nullptr;
};

FSTSMipBuilder::FSTSMipBuilder(int32 InThreadCount)
	: ThreadCount(FMath::Clamp(InThreadCount, 1, 16))
{
	Workers.Reserve(ThreadCount);
	for (int32 Index = 0; Index < ThreadCount; ++Index)
	{
		Workers.Add(MakeUnique<FWorker>(*this, Index));
	}
}

FSTSMipBuilder::~FSTSMipBuilder()
{
	Shutdown();
}

void FSTSMipBuilder::Shutdown()
{
	bStopping.store(true, std::memory_order_release);
	for (const TUniquePtr<FWorker>& Worker : Workers)
	{
		Worker->Wake();
	}
	Workers.Reset();

	FSTSBuildJob Job;
	while (Finished.Dequeue(Job))
	{
		Job = FSTSBuildJob();
	}
	{
		FScopeLock Lock(&JobLock);
		Jobs.Empty();
	}
	Outstanding = 0;
}

void FSTSMipBuilder::SetEpoch(uint32 NewEpoch)
{
	Epoch.store(NewEpoch, std::memory_order_release);
}

void FSTSMipBuilder::Submit(FSTSBuildJob&& Job)
{
	if (bStopping.load(std::memory_order_acquire))
	{
		return;
	}

	++Outstanding;
	{
		FScopeLock Lock(&JobLock);
		Jobs.EmplaceLast(MoveTemp(Job));
	}
	for (const TUniquePtr<FWorker>& Worker : Workers)
	{
		Worker->Wake();
	}
}

void FSTSMipBuilder::Drain(TArray<FSTSBuildJob>& OutFinished)
{
	FSTSBuildJob Job;
	while (Finished.Dequeue(Job))
	{
		--Outstanding;
		OutFinished.Add(MoveTemp(Job));
		Job = FSTSBuildJob();
	}
}

bool FSTSMipBuilder::TryTakeJob(FSTSBuildJob& OutJob)
{
	FScopeLock Lock(&JobLock);
	if (Jobs.IsEmpty())
	{
		return false;
	}
	OutJob = MoveTemp(Jobs.First());
	Jobs.PopFirst();
	return true;
}

void FSTSMipBuilder::Process(FSTSBuildJob& Job)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(STS_BuildMips);
	const double StartSec = FPlatformTime::Seconds();

	if (Job.Request.TileEpoch != Epoch.load(std::memory_order_acquire) || !Job.Source.IsValid())
	{
		Job.bBuildSkipped = true;
	}
	else
	{
		const TArray<FSTSMipRange>& Ranges = Job.Source->Mips;
		const int32 Resolution = Job.Request.Resolution;
		Job.Mips.Reserve(Ranges.Num());
		for (int32 MipIndex = 0; MipIndex < Ranges.Num(); ++MipIndex)
		{
			const FSTSMipRange& Range = Ranges[MipIndex];
			const int32 MipSize = FMath::Max(1, Resolution >> MipIndex);

			TUniquePtr<FTexture2DMipMap> Mip = MakeUnique<FTexture2DMipMap>(MipSize, MipSize, 1);
			Mip->BulkData.Lock(LOCK_READ_WRITE);
			void* Dest = Mip->BulkData.Realloc(Range.Size);
			FMemory::Memcpy(Dest, Job.Buffer.GetData() + Range.Offset, Range.Size);
			Mip->BulkData.Unlock();
			if (Job.bSingleUse)
			{
				// FBulkData::Unlock frees single-use data, so the flag goes on after the fill.
				Mip->BulkData.SetBulkDataFlags(BULKDATA_SingleUse);
			}
			Job.Mips.Add(MoveTemp(Mip));
		}
	}

	Job.Buffer = FSTSPayloadBuffer();
	Job.BuildMs = (FPlatformTime::Seconds() - StartSec) * 1000.0;
	Finished.Enqueue(MoveTemp(Job));
}

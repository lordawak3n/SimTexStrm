// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "STSTypes.h"
#include "Tasks/Pipe.h"

/** One CSV row. CacheHits is still reserved for a later phase; it is written as 0 and not stored. */
struct FSTSFrameSample
{
	// Phase 0 columns.
	uint64 Frame = 0;
	double TimeSec = 0.0;
	float FrameMs = 0.0f;
	float GameThreadMs = 0.0f;
	float RenderThreadMs = 0.0f;
	float RHIThreadMs = 0.0f;
	float GpuMs = 0.0f;
	int32 TilesAlive = 0;
	float TexMB[STS::NumBuckets] = {};
	float ProcessPhysMB = 0.0f;
	float RHITextureMB = 0.0f;

	// Phase 1 columns. Counts and times are per frame; PendingRequests is the value at the end of the frame.
	int32 PendingRequests = 0;
	int32 RequestsIssued = 0;
	int32 ResponsesReleased = 0;
	float ResponseMB = 0.0f;
	float ResponseLatencyAvgMs = 0.0f;
	float ResponseLatencyMaxMs = 0.0f;
	int32 StaleDiscarded = 0;
	int32 ChecksumFailures = 0;
	float ConsumerMs = 0.0f;
	float ChecksumMs = 0.0f;
	float IssueMs = 0.0f;

	// Phase 2 columns. Uploads and UploadMB sit in the columns reserved for them; the rest are appended.
	int32 Uploads = 0;
	float UploadMB = 0.0f;
	float BuildMs = 0.0f;
	int32 BuildQueueDepth = 0;
	float ApplyGTMs = 0.0f;
	float UpdateResourceGTMs = 0.0f;
	/** Render-thread time between the markers around UpdateResource; the samples arrive about one frame late. */
	float RTCreateMs = 0.0f;
	int32 FreeObjects[STS::NumBuckets] = {};
	int32 MinReuseAgeFrames = -1;
	int32 UnsafeReuses = 0;
	int32 PoolExhausted = 0;
	float IssueToVisibleAvgMs = 0.0f;
	float IssueToVisibleMaxMs = 0.0f;
	float ReleaseToVisibleMaxMs = 0.0f;
	/** Negative when the RHI is not D3D12 or the query failed. */
	float GpuLocalMemMB = -1.0f;
};

struct FSTSColumnStats
{
	int32 Count = 0;
	double Average = 0.0;
	double P50 = 0.0;
	double P99 = 0.0;
	double P999 = 0.0;
	double Max = 0.0;
	int32 Over16_67 = 0;
	int32 Over33_33 = 0;
};

/**
 * Buffers per-frame samples and streams them to a CSV on a worker pipe, so recording does no file I/O on the game thread.
 * Timing columns are also kept in memory for the percentile summary written by Stop.
 */
class FSTSRecorder
{
public:
	static constexpr int32 NumTimingColumns = 5;

	FSTSRecorder();
	~FSTSRecorder();

	bool Start(const FString& InName, double InDurationSec, double NowSec, const FString& InOutputDir);

	/** Flushes the CSV, writes the summary and returns its path. SummaryHeaderLines are inserted before the statistics table. */
	FString Stop(const TArray<FString>& SummaryHeaderLines);

	bool IsRecording() const { return bRecording; }
	const FString& GetName() const { return Name; }
	uint64 GetFrameCount() const { return FrameCount; }
	double GetElapsedSec(double NowSec) const { return NowSec - StartTimeSec; }
	bool IsDurationElapsed(double NowSec) const { return bRecording && DurationSec > 0.0 && NowSec - StartTimeSec >= DurationSec; }
	double GetDurationSec() const { return DurationSec; }

	void AddSample(double NowSec, const FSTSFrameSample& Sample);

	static const TCHAR* GetTimingColumnName(int32 ColumnIndex);
	static FSTSColumnStats ComputeStats(TConstArrayView<float> Values);

private:
	void FlushPending();

	bool bRecording = false;
	FString Name;
	FString CsvPath;
	FString SummaryPath;
	FString StartTimestamp;
	double DurationSec = 0.0;
	double StartTimeSec = 0.0;
	uint64 FrameCount = 0;

	TArray<float> Columns[NumTimingColumns];
	TArray<FSTSFrameSample> Pending;

	TUniquePtr<FArchive> CsvWriter;
	TUniquePtr<UE::Tasks::FPipe> WritePipe;
};

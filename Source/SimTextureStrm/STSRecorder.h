// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "STSTypes.h"
#include "Tasks/Pipe.h"

/** One CSV row. Columns reserved for later phases are written as 0 and are not stored. */
struct FSTSFrameSample
{
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

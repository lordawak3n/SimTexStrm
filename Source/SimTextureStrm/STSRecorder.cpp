// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSRecorder.h"

#include "SimTextureStrm.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "Serialization/Archive.h"
#include "Containers/StringConv.h"

namespace
{
	constexpr int32 FlushRowCount = 16384;
	constexpr int32 InitialColumnReserve = 262144;
	constexpr float FrameBudgetMs60 = 16.67f;
	constexpr float FrameBudgetMs30 = 33.33f;

	const TCHAR* const CsvHeader = TEXT("Frame,TimeSec,FrameMs,GameThreadMs,RenderThreadMs,RHIThreadMs,GpuMs,TilesAlive,TexMB_512,TexMB_1k,TexMB_2k,TexMB_4k,ProcessPhysMB,RHITextureMB,Uploads,UploadMB,CacheHits,PendingRequests,RequestsIssued,ResponsesReleased,ResponseMB,ResponseLatencyAvgMs,ResponseLatencyMaxMs,StaleDiscarded,ChecksumFailures,ConsumerMs,ChecksumMs,IssueMs,BuildMs,BuildQueueDepth,ApplyGTMs,UpdateResourceGTMs,RTCreateMs,Free512,Free1k,Free2k,Free4k,MinReuseAgeFrames,UnsafeReuses,PoolExhausted,IssueToVisibleAvgMs,IssueToVisibleMaxMs,ReleaseToVisibleMaxMs,GpuLocalMemMB\n");

	void WriteUtf8(FArchive& Writer, const FString& Text)
	{
		FTCHARToUTF8 Converted(*Text);
		Writer.Serialize(const_cast<ANSICHAR*>(Converted.Get()), Converted.Length());
	}
}

FSTSRecorder::FSTSRecorder() = default;

FSTSRecorder::~FSTSRecorder()
{
	if (WritePipe)
	{
		WritePipe->WaitUntilEmpty();
	}
}

const TCHAR* FSTSRecorder::GetTimingColumnName(int32 ColumnIndex)
{
	static const TCHAR* const Names[NumTimingColumns] = { TEXT("FrameMs"), TEXT("GameThreadMs"), TEXT("RenderThreadMs"), TEXT("RHIThreadMs"), TEXT("GpuMs") };
	return Names[ColumnIndex];
}

FSTSColumnStats FSTSRecorder::ComputeStats(TConstArrayView<float> Values)
{
	FSTSColumnStats Stats;
	Stats.Count = Values.Num();
	if (Stats.Count == 0)
	{
		return Stats;
	}

	TArray<float> Sorted;
	Sorted.Append(Values.GetData(), Values.Num());
	Sorted.Sort();

	double Sum = 0.0;
	for (const float Value : Sorted)
	{
		Sum += Value;
		Stats.Over16_67 += Value > FrameBudgetMs60 ? 1 : 0;
		Stats.Over33_33 += Value > FrameBudgetMs30 ? 1 : 0;
	}

	// Nearest-rank percentile.
	const auto Percentile = [&Sorted](double Fraction)
	{
		const int32 Index = FMath::Clamp(FMath::CeilToInt(Fraction * Sorted.Num()) - 1, 0, Sorted.Num() - 1);
		return double(Sorted[Index]);
	};

	Stats.Average = Sum / Stats.Count;
	Stats.P50 = Percentile(0.50);
	Stats.P99 = Percentile(0.99);
	Stats.P999 = Percentile(0.999);
	Stats.Max = Sorted.Last();
	return Stats;
}

bool FSTSRecorder::Start(const FString& InName, double InDurationSec, double NowSec, const FString& InOutputDir)
{
	if (bRecording)
	{
		return false;
	}

	IFileManager::Get().MakeDirectory(*InOutputDir, true);

	FString SafeName = FPaths::MakeValidFileName(InName);
	if (SafeName.IsEmpty())
	{
		SafeName = TEXT("Run");
	}

	StartTimestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString BasePath = InOutputDir / FString::Printf(TEXT("%s_%s"), *SafeName, *StartTimestamp);
	CsvPath = BasePath + TEXT(".csv");
	SummaryPath = BasePath + TEXT("_summary.txt");

	CsvWriter.Reset(IFileManager::Get().CreateFileWriter(*CsvPath, FILEWRITE_AllowRead));
	if (!CsvWriter)
	{
		UE_LOG(LogSTS, Error, TEXT("Recorder: cannot create '%s'."), *CsvPath);
		return false;
	}
	WriteUtf8(*CsvWriter, CsvHeader);

	WritePipe = MakeUnique<UE::Tasks::FPipe>(UE_SOURCE_LOCATION);

	Name = SafeName;
	DurationSec = FMath::Max(0.0, InDurationSec);
	StartTimeSec = NowSec;
	FrameCount = 0;

	const int32 Reserve = DurationSec > 0.0 ? int32(FMath::Clamp(DurationSec * 2000.0, 1024.0, 8.0e6)) : InitialColumnReserve;
	for (TArray<float>& Column : Columns)
	{
		Column.Reset(Reserve);
	}
	Pending.Reset(FlushRowCount);

	bRecording = true;
	TRACE_BOOKMARK(TEXT("STS Record Start: %s"), *Name);
	UE_LOG(LogSTS, Log, TEXT("Recorder: started '%s' (duration %.1f s) -> %s"), *Name, DurationSec, *CsvPath);
	return true;
}

void FSTSRecorder::AddSample(double NowSec, const FSTSFrameSample& Sample)
{
	if (!bRecording)
	{
		return;
	}

	FSTSFrameSample& Row = Pending.Add_GetRef(Sample);
	Row.Frame = FrameCount++;
	Row.TimeSec = NowSec - StartTimeSec;

	Columns[0].Add(Row.FrameMs);
	Columns[1].Add(Row.GameThreadMs);
	Columns[2].Add(Row.RenderThreadMs);
	Columns[3].Add(Row.RHIThreadMs);
	Columns[4].Add(Row.GpuMs);

	if (Pending.Num() >= FlushRowCount)
	{
		FlushPending();
	}
}

void FSTSRecorder::FlushPending()
{
	if (Pending.IsEmpty() || !WritePipe)
	{
		return;
	}

	TArray<FSTSFrameSample> Chunk = MoveTemp(Pending);
	Pending.Reset(FlushRowCount);

	FArchive* Writer = CsvWriter.Get();
	WritePipe->Launch(UE_SOURCE_LOCATION, [Writer, Chunk = MoveTemp(Chunk)]()
	{
		FString Text;
		Text.Reserve(Chunk.Num() * 256);
		for (const FSTSFrameSample& Row : Chunk)
		{
			Text.Appendf(TEXT("%llu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.3f,%.3f,%.3f,%.3f,%.1f,%.1f,%d,%.3f,0,%d,%d,%d,%.3f,%.3f,%.3f,%d,%d,%.4f,%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.1f\n"),
				Row.Frame, Row.TimeSec, Row.FrameMs, Row.GameThreadMs, Row.RenderThreadMs, Row.RHIThreadMs, Row.GpuMs,
				Row.TilesAlive, Row.TexMB[0], Row.TexMB[1], Row.TexMB[2], Row.TexMB[3], Row.ProcessPhysMB, Row.RHITextureMB,
				Row.Uploads, Row.UploadMB,
				Row.PendingRequests, Row.RequestsIssued, Row.ResponsesReleased, Row.ResponseMB, Row.ResponseLatencyAvgMs, Row.ResponseLatencyMaxMs,
				Row.StaleDiscarded, Row.ChecksumFailures, Row.ConsumerMs, Row.ChecksumMs, Row.IssueMs,
				Row.BuildMs, Row.BuildQueueDepth, Row.ApplyGTMs, Row.UpdateResourceGTMs, Row.RTCreateMs,
				Row.FreeObjects[0], Row.FreeObjects[1], Row.FreeObjects[2], Row.FreeObjects[3],
				Row.MinReuseAgeFrames, Row.UnsafeReuses, Row.PoolExhausted,
				Row.IssueToVisibleAvgMs, Row.IssueToVisibleMaxMs, Row.ReleaseToVisibleMaxMs, Row.GpuLocalMemMB);
		}
		WriteUtf8(*Writer, Text);
	});
}

FString FSTSRecorder::Stop(const TArray<FString>& SummaryHeaderLines)
{
	if (!bRecording)
	{
		return FString();
	}

	const double ElapsedSec = FPlatformTime::Seconds() - StartTimeSec;
	bRecording = false;

	FlushPending();
	WritePipe->WaitUntilEmpty();
	WritePipe.Reset();
	CsvWriter.Reset();
	TRACE_BOOKMARK(TEXT("STS Record Stop: %s"), *Name);

	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("Run: %s"), *Name));
	Lines.Add(FString::Printf(TEXT("Started: %s"), *StartTimestamp));
	Lines.Add(FString::Printf(TEXT("Duration: %.3f s, frames: %llu"), ElapsedSec, FrameCount));
	Lines.Add(FString::Printf(TEXT("CSV: %s"), *CsvPath));
	Lines.Append(SummaryHeaderLines);
	Lines.Add(FString());
	Lines.Add(TEXT("Timing statistics (ms; percentiles are nearest-rank):"));
	Lines.Add(FString::Printf(TEXT("%-16s %9s %9s %9s %9s %9s %9s %9s %9s"), TEXT("Column"), TEXT("Samples"), TEXT("Average"), TEXT("P50"), TEXT("P99"), TEXT("P99.9"), TEXT("Max"), TEXT(">16.67ms"), TEXT(">33.33ms")));
	for (int32 ColumnIndex = 0; ColumnIndex < NumTimingColumns; ++ColumnIndex)
	{
		const FSTSColumnStats Stats = ComputeStats(Columns[ColumnIndex]);
		Lines.Add(FString::Printf(TEXT("%-16s %9d %9.3f %9.3f %9.3f %9.3f %9.3f %9d %9d"),
			GetTimingColumnName(ColumnIndex), Stats.Count, Stats.Average, Stats.P50, Stats.P99, Stats.P999, Stats.Max, Stats.Over16_67, Stats.Over33_33));
	}

	FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")) + TEXT("\n"), *SummaryPath);

	UE_LOG(LogSTS, Log, TEXT("Recorder: stopped '%s'. Summary: %s"), *Name, *SummaryPath);
	for (const FString& Line : Lines)
	{
		UE_LOG(LogSTS, Log, TEXT("  %s"), *Line);
	}

	for (TArray<float>& Column : Columns)
	{
		Column.Empty();
	}
	Pending.Empty();
	return SummaryPath;
}

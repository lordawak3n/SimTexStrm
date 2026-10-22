// Copyright Epic Games, Inc. All Rights Reserved.

#include "STSTestbench.h"

#include "SimTextureStrm.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/MiscTrace.h"

namespace
{
	constexpr float PresetSettleSec = 5.0f;
	constexpr float PresetIdleTimeoutSec = 10.0f;
	constexpr int32 MaxInstantStepsPerFrame = 256;
}

void ASTSTestbench::BuildScenarioPreset(ESTSScenarioPreset Preset, TArray<FSTSScenarioStep>& OutSteps) const
{
	OutSteps.Reset();

	const auto Add = [&OutSteps](ESTSScenarioStepType Type) -> FSTSScenarioStep&
	{
		FSTSScenarioStep& Step = OutSteps.AddDefaulted_GetRef();
		Step.Type = Type;
		return Step;
	};
	const auto AddWait = [&Add](float Sec)
	{
		Add(ESTSScenarioStepType::Wait).DurationSec = Sec;
	};
	const auto AddPattern = [&Add](ESTSUpdatePattern Pattern)
	{
		Add(ESTSScenarioStepType::SetPattern).Pattern = Pattern;
	};
	const auto AddBurst = [&Add](int32 N4k, int32 N2k, int32 N1k, int32 N512)
	{
		FSTSScenarioStep& Step = Add(ESTSScenarioStepType::Burst);
		Step.Burst4k = N4k;
		Step.Burst2k = N2k;
		Step.Burst1k = N1k;
		Step.Burst512 = N512;
	};
	const auto AddRecordStart = [&Add](const TCHAR* Name)
	{
		Add(ESTSScenarioStepType::RecordStart).Name = Name;
	};
	const auto AddSpreadStart = [&Add](float Rate, float DurationSec)
	{
		FSTSScenarioStep& Step = Add(ESTSScenarioStepType::SpreadStart);
		Step.Rate = Rate;
		Step.DurationSec = DurationSec;
	};
	const auto AddFinish = [&Add]()
	{
		Add(ESTSScenarioStepType::WaitForIdle).DurationSec = PresetIdleTimeoutSec;
		Add(ESTSScenarioStepType::RecordStop);
	};

	// Every preset starts the same way: tiles, a settle, then the update knobs fixed so runs repeat.
	const auto AddPrologue = [&](ESTSUpdatePattern Pattern, ESTSTargetMode Mode)
	{
		Add(ESTSScenarioStepType::CreateTiles);
		AddWait(PresetSettleSec);
		AddPattern(Pattern);
		Add(ESTSScenarioStepType::SetTargetMode).TargetMode = Mode;
		Add(ESTSScenarioStepType::SetSeed).Seed = UpdateSeed;
	};

	switch (Preset)
	{
	case ESTSScenarioPreset::SteadySpread:
		AddPrologue(ESTSUpdatePattern::Mixed, ESTSTargetMode::Random);
		AddRecordStart(TEXT("SteadySpread"));
		AddSpreadStart(5.0f, 60.0f);
		AddWait(60.0f);
		Add(ESTSScenarioStepType::SpreadStop);
		AddFinish();
		break;

	case ESTSScenarioPreset::SpreadSweep:
		AddPrologue(ESTSUpdatePattern::Mixed, ESTSTargetMode::Random);
		for (const int32 Rate : { 5, 10, 25, 50 })
		{
			Add(ESTSScenarioStepType::SetSeed).Seed = UpdateSeed;
			AddRecordStart(*FString::Printf(TEXT("SpreadSweep_%d"), Rate));
			AddSpreadStart(float(Rate), 30.0f);
			AddWait(30.0f);
			Add(ESTSScenarioStepType::SpreadStop);
			AddFinish();
			AddWait(2.0f);
		}
		break;

	case ESTSScenarioPreset::Burst:
		AddPrologue(ESTSUpdatePattern::StepUp, ESTSTargetMode::Random);
		AddRecordStart(TEXT("Burst"));
		AddWait(5.0f);
		AddBurst(20, 40, 0, 0);
		AddWait(10.0f);
		// Counts are by target resolution, so the StepDown burst moves the same number of tiles back: 20 to 2k and 40 to 1k.
		AddPattern(ESTSUpdatePattern::StepDown);
		AddBurst(0, 20, 40, 0);
		AddWait(10.0f);
		AddPattern(ESTSUpdatePattern::StepUp);
		AddBurst(20, 40, 0, 0);
		AddWait(10.0f);
		AddFinish();
		break;

	case ESTSScenarioPreset::Oscillation:
	{
		int32 CellsX = 0;
		int32 CellsY = 0;
		ResolveGrid(CellsX, CellsY, false);
		const int32 PatchX = FMath::Min(6, CellsX);
		const int32 PatchY = FMath::Min(6, CellsY);

		AddPrologue(ESTSUpdatePattern::Oscillate, ESTSTargetMode::Patch);
		FSTSScenarioStep& Mode = OutSteps[OutSteps.Num() - 2];
		Mode.PatchCenter = FIntPoint(CellsX / 2, CellsY / 2);
		Mode.PatchSize = FIntPoint(PatchX, PatchY);
		Mode.PatchStep = FIntPoint(0, 0);

		AddRecordStart(TEXT("Oscillation"));
		FSTSScenarioStep& Repeat = Add(ESTSScenarioStepType::BurstRepeat);
		Repeat.Rate = 2.0f;
		Repeat.DurationSec = 60.0f;
		Repeat.Burst4k = 2 * PatchX * PatchY;
		AddWait(60.0f);
		Add(ESTSScenarioStepType::SpreadStop);
		AddFinish();
		break;
	}

	case ESTSScenarioPreset::Soak:
		AddPrologue(ESTSUpdatePattern::Mixed, ESTSTargetMode::Random);
		AddRecordStart(TEXT("Soak"));
		AddSpreadStart(10.0f, 0.0f);
		for (int32 Minute = 0; Minute < 30; ++Minute)
		{
			AddWait(60.0f);
			AddBurst(5, 10, 10, 10);
		}
		Add(ESTSScenarioStepType::SpreadStop);
		AddFinish();
		break;

	case ESTSScenarioPreset::Overhead:
		AddPrologue(ESTSUpdatePattern::Mixed, ESTSTargetMode::Random);
		AddRecordStart(TEXT("Overhead_Idle"));
		AddWait(30.0f);
		Add(ESTSScenarioStepType::RecordStop);
		AddWait(2.0f);
		Add(ESTSScenarioStepType::SetSeed).Seed = UpdateSeed;
		AddRecordStart(TEXT("Overhead_Spread50"));
		AddSpreadStart(50.0f, 30.0f);
		AddWait(30.0f);
		Add(ESTSScenarioStepType::SpreadStop);
		AddFinish();
		break;

	case ESTSScenarioPreset::Lifetime:
		AddPrologue(ESTSUpdatePattern::StepUp, ESTSTargetMode::Random);
		AddRecordStart(TEXT("Lifetime"));
		for (int32 Round = 0; Round < 3; ++Round)
		{
			AddBurst(20, 40, 0, 0);
			AddWait(0.005f);
			Add(ESTSScenarioStepType::DestroyTiles);
			Add(ESTSScenarioStepType::CreateTiles);
			AddWait(1.0f);
		}
		AddBurst(20, 40, 0, 0);
		AddFinish();
		break;

	case ESTSScenarioPreset::Custom:
		return;
	}

	Add(ESTSScenarioStepType::Quit);
}

void ASTSTestbench::ApplyScenarioPreset()
{
	if (ScenarioPreset == ESTSScenarioPreset::Custom)
	{
		UE_LOG(LogSTS, Log, TEXT("ApplyScenarioPreset: Custom keeps the current ScenarioSteps (%d)."), ScenarioSteps.Num());
		return;
	}
	BuildScenarioPreset(ScenarioPreset, ScenarioSteps);
	UE_LOG(LogSTS, Log, TEXT("Applied scenario preset %s: %d steps."), *STS::EnumToString(ScenarioPreset), ScenarioSteps.Num());
}

void ASTSTestbench::RunScenario()
{
	if (!IsGameWorldValid(TEXT("RunScenario")))
	{
		return;
	}
	if (bScenarioRunning || SequenceStep != ESTSSequenceStep::Idle)
	{
		UE_LOG(LogSTS, Warning, TEXT("RunScenario ignored: a scenario or the Phase 0 sequence is already running."));
		return;
	}
	if (Recorder.IsRecording())
	{
		UE_LOG(LogSTS, Warning, TEXT("RunScenario ignored: stop the active recording first."));
		return;
	}
	if (ScenarioSteps.IsEmpty())
	{
		UE_LOG(LogSTS, Warning, TEXT("RunScenario ignored: ScenarioSteps is empty. Use ApplyScenarioPreset or fill the steps."));
		return;
	}

	bAutoRecordPending = false;
	Spread = FTimeline();
	BurstRepeat = FTimeline();

	// Meshes, payloads and the server library are prepared now so no step's measured frames include their generation.
	ResolveBaseMaterial();
	EnsureMeshes();
	TSet<int32> Needed;
	for (const int32 Resolution : STS::BucketResolutions)
	{
		Needed.Add(Resolution);
	}
	if (STS::ResolutionToBucket(RemainderResolution) != INDEX_NONE)
	{
		Needed.Add(RemainderResolution);
	}
	EnsurePayloadLibrary(Needed);
	EnsureServer();
	EnsureServerLibrary();

	UpdateStream.Initialize(UpdateSeed);
	PatchTriggerCount = 0;

	ScenarioRuntimeSteps = ScenarioSteps;
	ScenarioStepIndex = 0;
	bScenarioStepEntered = false;
	bScenarioRunning = true;
	UE_LOG(LogSTS, Log, TEXT("Scenario: running %d steps."), ScenarioRuntimeSteps.Num());
	TRACE_BOOKMARK(TEXT("STS Scenario Start"));
}

void ASTSTestbench::StopScenario()
{
	if (!bScenarioRunning)
	{
		UE_LOG(LogSTS, Log, TEXT("StopScenario: no scenario is running."));
		return;
	}
	Spread.bActive = false;
	BurstRepeat.bActive = false;
	bScenarioQuitWhenDone = false;
	FinishScenario(TEXT("stopped"));
}

void ASTSTestbench::FinishScenario(const TCHAR* Reason)
{
	bScenarioRunning = false;
	UE_LOG(LogSTS, Log, TEXT("Scenario: %s. Output is in %s"), Reason, *FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("SimTexStrm")));
	TRACE_BOOKMARK(TEXT("STS Scenario End: %s"), Reason);
	if (bScenarioQuitWhenDone)
	{
		bScenarioQuitWhenDone = false;
		RequestQuit(TEXT("STS scenario complete"));
	}
}

void ASTSTestbench::RequestQuit(const TCHAR* Reason)
{
	if (GIsEditor)
	{
		UE_LOG(LogSTS, Log, TEXT("Quit ignored: running inside the editor. End PIE manually."));
		return;
	}
	UE_LOG(LogSTS, Log, TEXT("Quitting: %s."), Reason);
	FPlatformMisc::RequestExit(false, Reason);
}

void ASTSTestbench::UpdateScenario(double NowSec)
{
	for (int32 Guard = 0; Guard < MaxInstantStepsPerFrame && bScenarioRunning; ++Guard)
	{
		if (!ScenarioRuntimeSteps.IsValidIndex(ScenarioStepIndex))
		{
			FinishScenario(TEXT("complete"));
			return;
		}

		const FSTSScenarioStep Step = ScenarioRuntimeSteps[ScenarioStepIndex];
		const bool bFirstFrame = !bScenarioStepEntered;
		if (bFirstFrame)
		{
			bScenarioStepEntered = true;
			ScenarioStepStartSec = NowSec;
			UE_LOG(LogSTS, Log, TEXT("Scenario: step %d/%d %s"), ScenarioStepIndex + 1, ScenarioRuntimeSteps.Num(), *STS::EnumToString(Step.Type));
			TRACE_BOOKMARK(TEXT("STS Scenario Step %d: %s"), ScenarioStepIndex + 1, *STS::EnumToString(Step.Type));
		}

		if (!ExecuteScenarioStep(Step, bFirstFrame, NowSec))
		{
			return;
		}
		if (!bScenarioRunning)
		{
			return;
		}
		++ScenarioStepIndex;
		bScenarioStepEntered = false;
	}
}

bool ASTSTestbench::ExecuteScenarioStep(const FSTSScenarioStep& Step, bool bFirstFrame, double NowSec)
{
	const double StepElapsedSec = NowSec - ScenarioStepStartSec;

	switch (Step.Type)
	{
	case ESTSScenarioStepType::Wait:
		return StepElapsedSec >= Step.DurationSec;

	case ESTSScenarioStepType::WaitForIdle:
		if (IsServerIdle())
		{
			return true;
		}
		if (Step.DurationSec > 0.0f && StepElapsedSec >= Step.DurationSec)
		{
			UE_LOG(LogSTS, Warning, TEXT("Scenario: WaitForIdle timed out after %.1f s with %d requests still in flight."), Step.DurationSec, Server->GetInFlight());
			return true;
		}
		return false;

	case ESTSScenarioStepType::CreateTiles:
		if (bFirstFrame)
		{
			if (Tiles.IsEmpty() && !bCreationInProgress)
			{
				CreateTiles();
			}
			else
			{
				UE_LOG(LogSTS, Log, TEXT("Scenario: tiles already exist or are being created; CreateTiles skipped."));
			}
		}
		return !bCreationInProgress;

	case ESTSScenarioStepType::DestroyTiles:
		DestroyTiles();
		return true;

	case ESTSScenarioStepType::RecordStart:
		StartRecordingNamed(Step.Name.IsEmpty() ? FString(TEXT("Scenario")) : Step.Name, Step.DurationSec);
		return true;

	case ESTSScenarioStepType::RecordStop:
		if (Recorder.IsRecording())
		{
			StopRecordingInternal();
		}
		return true;

	case ESTSScenarioStepType::SetPattern:
		UpdatePattern = Step.Pattern;
		if (Step.Pattern == ESTSUpdatePattern::Mixed)
		{
			MixedWeightSame = float(Step.MixedWeights.X);
			MixedWeightUp = float(Step.MixedWeights.Y);
			MixedWeightDown = float(Step.MixedWeights.Z);
		}
		return true;

	case ESTSScenarioStepType::SetTargetMode:
		TargetMode = Step.TargetMode;
		RankMin = Step.RankRange.X;
		RankMax = Step.RankRange.Y;
		PatchCenterX = Step.PatchCenter.X;
		PatchCenterY = Step.PatchCenter.Y;
		PatchSizeX = FMath::Max(1, Step.PatchSize.X);
		PatchSizeY = FMath::Max(1, Step.PatchSize.Y);
		PatchStepX = Step.PatchStep.X;
		PatchStepY = Step.PatchStep.Y;
		PatchTriggerCount = 0;
		return true;

	case ESTSScenarioStepType::SetSeed:
		UpdateSeed = Step.Seed;
		UpdateStream.Initialize(UpdateSeed);
		return true;

	case ESTSScenarioStepType::Burst:
		TriggerBurstCounts(Step.Burst4k, Step.Burst2k, Step.Burst1k, Step.Burst512);
		return true;

	case ESTSScenarioStepType::SpreadStart:
		Spread.bActive = false;
		StartSpreadRate(Step.Rate, Step.DurationSec);
		return true;

	case ESTSScenarioStepType::SpreadStop:
		StopSpread();
		return true;

	case ESTSScenarioStepType::BurstRepeat:
		if (Step.Rate <= 0.0f)
		{
			UE_LOG(LogSTS, Warning, TEXT("Scenario: BurstRepeat needs a rate above 0."));
			return true;
		}
		BurstRepeat = FTimeline();
		BurstRepeat.bActive = true;
		BurstRepeat.StartSec = NowSec;
		BurstRepeat.Rate = Step.Rate;
		BurstRepeat.DurationSec = Step.DurationSec;
		BurstRepeat.BurstCounts[0] = Step.Burst512;
		BurstRepeat.BurstCounts[1] = Step.Burst1k;
		BurstRepeat.BurstCounts[2] = Step.Burst2k;
		BurstRepeat.BurstCounts[3] = Step.Burst4k;
		return true;

	case ESTSScenarioStepType::Quit:
		bScenarioQuitWhenDone = true;
		FinishScenario(TEXT("complete (Quit step)"));
		return true;
	}
	return true;
}

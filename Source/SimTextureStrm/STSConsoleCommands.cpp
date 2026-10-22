// Copyright Epic Games, Inc. All Rights Reserved.

#include "SimTextureStrm.h"
#include "STSTestbench.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

namespace
{
	ASTSTestbench* FindTestbench(UWorld* World, const TCHAR* Command)
	{
		if (World)
		{
			for (TActorIterator<ASTSTestbench> It(World); It; ++It)
			{
				return *It;
			}
		}
		UE_LOG(LogSTS, Error, TEXT("%s: no ASTSTestbench in the world."), Command);
		return nullptr;
	}

	FAutoConsoleCommandWithWorldAndArgs CreateCommand(
		TEXT("sts.Tiles.Create"),
		TEXT("Creates the tile field with the testbench's current knobs."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Tiles.Create")))
			{
				Testbench->CreateTiles();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs DestroyCommand(
		TEXT("sts.Tiles.Destroy"),
		TEXT("Destroys the tile field."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Tiles.Destroy")))
			{
				Testbench->DestroyTiles();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs RebuildCommand(
		TEXT("sts.Tiles.Rebuild"),
		TEXT("Destroys the tile field, then creates it again with the current knobs."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Tiles.Rebuild")))
			{
				Testbench->RebuildTiles();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs RecordStartCommand(
		TEXT("sts.Record.Start"),
		TEXT("sts.Record.Start [Name] [DurationSec]: starts a recording. Defaults come from the testbench; a duration of 0 records until sts.Record.Stop."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Record.Start"));
			if (!Testbench)
			{
				return;
			}
			const FString Name = Args.IsValidIndex(0) ? Args[0] : Testbench->RecordName;
			const double DurationSec = Args.IsValidIndex(1) ? FCString::Atod(*Args[1]) : double(Testbench->RecordDurationSec);
			Testbench->StartRecordingNamed(Name, DurationSec);
		}));

	FAutoConsoleCommandWithWorldAndArgs RecordStopCommand(
		TEXT("sts.Record.Stop"),
		TEXT("Stops the active recording and writes the summary."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Record.Stop")))
			{
				Testbench->StopRecording();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs SequenceCommand(
		TEXT("sts.Seq.Phase0"),
		TEXT("Runs the Phase 0 sequence: warm-up, record Empty, create tiles, settle, record Tiles, optionally destroy and record AfterDestroy."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Seq.Phase0")))
			{
				Testbench->RunPhase0Sequence();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs HudCommand(
		TEXT("sts.Hud"),
		TEXT("sts.Hud [0|1]: shows or hides the HUD; without an argument it toggles."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Hud"));
			if (!Testbench)
			{
				return;
			}
			Testbench->SetHudVisible(Args.IsValidIndex(0) ? FCString::Atoi(*Args[0]) != 0 : !Testbench->IsHudVisible());
		}));

	FAutoConsoleCommandWithWorldAndArgs BurstCommand(
		TEXT("sts.Burst"),
		TEXT("sts.Burst [n4k n2k n1k n512]: issues a burst of update requests in one frame. Counts are by requested resolution; without arguments the Burst* knobs are used."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Burst"));
			if (!Testbench)
			{
				return;
			}
			if (Args.Num() == 0)
			{
				Testbench->TriggerBurst();
				return;
			}
			const auto Count = [&Args](int32 Index) { return Args.IsValidIndex(Index) ? FCString::Atoi(*Args[Index]) : 0; };
			Testbench->TriggerBurstCounts(Count(0), Count(1), Count(2), Count(3));
		}));

	FAutoConsoleCommandWithWorldAndArgs SpreadCommand(
		TEXT("sts.Spread"),
		TEXT("sts.Spread [Rate] [DurationSec]: spreads update requests over time. Defaults come from the testbench; a duration of 0 runs until sts.Spread.Stop."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Spread"));
			if (!Testbench)
			{
				return;
			}
			const double Rate = Args.IsValidIndex(0) ? FCString::Atod(*Args[0]) : double(Testbench->SpreadRatePerSec);
			const double DurationSec = Args.IsValidIndex(1) ? FCString::Atod(*Args[1]) : double(Testbench->SpreadDurationSec);
			Testbench->StartSpreadRate(Rate, DurationSec);
		}));

	FAutoConsoleCommandWithWorldAndArgs SpreadStopCommand(
		TEXT("sts.Spread.Stop"),
		TEXT("Stops the spread (and any repeating burst)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Spread.Stop")))
			{
				Testbench->StopSpread();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs ScenarioRunCommand(
		TEXT("sts.Scenario.Run"),
		TEXT("Runs the testbench's ScenarioSteps."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Scenario.Run")))
			{
				Testbench->RunScenario();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs ScenarioStopCommand(
		TEXT("sts.Scenario.Stop"),
		TEXT("Stops the running scenario without quitting."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Scenario.Stop")))
			{
				Testbench->StopScenario();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs ScenarioApplyCommand(
		TEXT("sts.Scenario.Apply"),
		TEXT("sts.Scenario.Apply <Preset>: fills ScenarioSteps from a preset (SteadySpread, SpreadSweep, Burst, Oscillation, Soak, Overhead, Lifetime, SoakShort)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Scenario.Apply"));
			if (!Testbench)
			{
				return;
			}
			if (Args.Num() == 0)
			{
				UE_LOG(LogSTS, Error, TEXT("sts.Scenario.Apply needs a preset name."));
				return;
			}
			Testbench->SetScenarioPresetByName(Args[0]);
		}));

	FAutoConsoleCommandWithWorldAndArgs PatternCommand(
		TEXT("sts.Pattern"),
		TEXT("sts.Pattern <SameResolution|StepUp|StepDown|Mixed|Oscillate>: sets the update pattern."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Pattern"));
			if (!Testbench)
			{
				return;
			}
			if (Args.Num() == 0)
			{
				UE_LOG(LogSTS, Error, TEXT("sts.Pattern needs a pattern name."));
				return;
			}
			Testbench->SetPatternByName(Args[0]);
		}));

	FAutoConsoleCommandWithWorldAndArgs ChecksumsCommand(
		TEXT("sts.Checksums"),
		TEXT("sts.Checksums [0|1]: enables or disables response checksum verification; without an argument it toggles."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Checksums"));
			if (!Testbench)
			{
				return;
			}
			Testbench->SetChecksumsEnabled(Args.IsValidIndex(0) ? FCString::Atoi(*Args[0]) != 0 : !Testbench->AreChecksumsEnabled());
		}));

	FAutoConsoleCommandWithWorldAndArgs CheckConsistencyCommand(
		TEXT("sts.Check.Consistency"),
		TEXT("Checks every tile (visual state equals logical state in every mode that updates visuals, texture size equals visual resolution, MID texture is the tile's texture, a Regions slot kept its GPU resource) and logs the mismatches."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Check.Consistency")))
			{
				Testbench->CheckConsistency();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CheckReadbackMipsCommand(
		TEXT("sts.Check.ReadbackMips"),
		TEXT("sts.Check.ReadbackMips [Resolution]: reads every mip of a few in-use slots per resolution back from the GPU and compares it with the payload the slot should hold. 0 or no argument checks every resolution. Stalls the GPU."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (ASTSTestbench* Testbench = FindTestbench(World, TEXT("sts.Check.ReadbackMips")))
			{
				Testbench->RunReadbackMips(Args.IsValidIndex(0) ? FCString::Atoi(*Args[0]) : 0);
			}
		}));
}

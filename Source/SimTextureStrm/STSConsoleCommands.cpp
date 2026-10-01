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
}

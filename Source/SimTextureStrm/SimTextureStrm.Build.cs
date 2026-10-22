// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class SimTextureStrm : ModuleRules
{
	public SimTextureStrm(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput" });

		// Slate: STSThreadTimes hooks the Slate renderer's present callback to read the thread times of the right present.
		PrivateDependencyModuleNames.AddRange(new string[] { "RenderCore", "RHI", "MeshDescription", "StaticMeshDescription", "Slate", "SlateCore" });

		// STSGpuMemory queries DXGI video memory usage on the adapter the D3D12 RHI uses.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("dxgi.lib");
		}

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}

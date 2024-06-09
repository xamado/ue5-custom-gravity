// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class CustomGravityTest : ModuleRules
{
	public CustomGravityTest(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", 
			"CoreUObject", 
			"Engine", 
			"InputCore", 
			"EnhancedInput", 
			"PerfCounters", 
            "NetCore",
			"PhysicsCore",
			"Chaos",
			"ChaosSolverEngine",
			"Iris",
			"IrisCore"
		});
	}
}

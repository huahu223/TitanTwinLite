// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

using UnrealBuildTool;

public class TitanTwinBinding : ModuleRules
{
	public TitanTwinBinding(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"TitanTwinRuntime"
			});

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"SlateCore",
				"UMG"
			});
	}
}

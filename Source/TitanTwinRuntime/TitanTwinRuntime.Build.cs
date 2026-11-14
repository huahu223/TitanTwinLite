// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

using UnrealBuildTool;

public class TitanTwinRuntime : ModuleRules
{
	public TitanTwinRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"DeveloperSettings",
				"Engine",
				"GameplayTags"
			}
		);
	}
}

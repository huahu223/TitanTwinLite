// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

using UnrealBuildTool;

public class TitanTwinMqtt : ModuleRules
{
	public TitanTwinMqtt(ReadOnlyTargetRules Target) : base(Target)
	{
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"TitanTwinRuntime",
				"TitanTwinDataSource"
			});

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"DeveloperSettings",
				"Json",
				"Sockets",
                "SSL"
			});
	}
}

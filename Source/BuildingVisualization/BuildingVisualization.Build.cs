// Copyright Matin. All Rights Reserved.

using UnrealBuildTool;

public class BuildingVisualization : ModuleRules
{
	public BuildingVisualization(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayTags",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
			"Renderer",
			"RHI",
			"Projects",
			"DeveloperSettings",
		});
	}
}

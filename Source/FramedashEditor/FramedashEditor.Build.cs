// Copyright 2026 Crane Valley. All Rights Reserved.

using UnrealBuildTool;
using System;

public class FramedashEditor : ModuleRules
{
	public FramedashEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"DeveloperSettings",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"HTTP",
			"InputCore",
			"Json",
			"LevelEditor",
			"RenderCore",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"UnrealEd",
		});

		// BuildPlugin emits development binaries, so WITH_DEV_AUTOMATION_TESTS alone
		// would package the specs. The dedicated automation job opts in explicitly.
		bool bBuildAutomationSpecs =
			Environment.GetEnvironmentVariable("FRAMEDASH_BUILD_AUTOMATION_SPECS") == "1";
		PrivateDefinitions.Add("FRAMEDASH_EDITOR_WITH_AUTOMATION_SPECS=" +
			(bBuildAutomationSpecs ? "1" : "0"));
	}
}

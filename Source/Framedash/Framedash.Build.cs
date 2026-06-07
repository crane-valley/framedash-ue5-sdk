// Copyright Crane Valley. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class Framedash : ModuleRules
{
	public Framedash(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// nanopb uses C99 features and may trigger warnings in MSVC
		CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off;

		string ThirdPartyPath = Path.Combine(ModuleDirectory, "Private", "ThirdParty");
		string ProtoPath = Path.Combine(ModuleDirectory, "Private", "Proto");

		PublicIncludePaths.AddRange(new string[] { });

		PrivateIncludePaths.AddRange(new string[] {
			Path.Combine(ThirdPartyPath, "nanopb"),
			ProtoPath,
		});

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"DeveloperSettings",
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"CoreUObject",
			"Engine",
			"HTTP",
			"Json",
			"RenderCore",
			"RHI",
		});

		// nanopb C files need special handling
		PublicDefinitions.Add("PB_FIELD_32BIT=1");
	}
}

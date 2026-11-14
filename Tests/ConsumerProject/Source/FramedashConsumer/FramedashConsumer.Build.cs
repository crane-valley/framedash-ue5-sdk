using UnrealBuildTool;

public class FramedashConsumer : ModuleRules
{
	public FramedashConsumer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "Framedash" });
	}
}

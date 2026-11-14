using UnrealBuildTool;

public class FramedashConsumerTarget : TargetRules
{
	public FramedashConsumerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("FramedashConsumer");
	}
}

using UnrealBuildTool;

public class FramedashConsumerEditorTarget : TargetRules
{
	public FramedashConsumerEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("FramedashConsumer");
	}
}

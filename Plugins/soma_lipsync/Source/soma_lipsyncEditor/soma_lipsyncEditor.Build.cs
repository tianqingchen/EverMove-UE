using UnrealBuildTool;

public class soma_lipsyncEditor : ModuleRules
{
	public soma_lipsyncEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"AnimationBlueprintLibrary",
			"AnimationCore",
			"soma_lipsync"
		});
	}
}

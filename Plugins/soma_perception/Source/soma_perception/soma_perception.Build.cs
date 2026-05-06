using System.IO;
using UnrealBuildTool;

public class soma_perception : ModuleRules
{
	public soma_perception(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RenderCore",
			"RHI",
			"Slate",
			"SlateCore",
			"HTTP",
			"Json",
			"JsonUtilities",
			"ImageWrapper",
			"AudioCaptureCore",
			"soma_storage"
		});

		// ONNX Runtime (CPU, Windows x64)
		string PluginDirectory = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		string OnnxRuntimeRoot = Path.Combine(PluginDirectory, "ThirdParty", "OnnxRuntime");

		string OnnxIncludePath = Path.Combine(OnnxRuntimeRoot, "include");
		string OnnxLibPath = Path.Combine(OnnxRuntimeRoot, "lib", "Win64");

		PublicSystemIncludePaths.Add(OnnxIncludePath);
		PublicAdditionalLibraries.Add(Path.Combine(OnnxLibPath, "onnxruntime.lib"));

		// Delay-load so editor/game can start even if the DLL is missing.
		PublicDelayLoadDLLs.Add("onnxruntime.dll");

		string OnnxDllPath = Path.Combine(OnnxLibPath, "onnxruntime.dll");
		RuntimeDependencies.Add(OnnxDllPath);
	}
}

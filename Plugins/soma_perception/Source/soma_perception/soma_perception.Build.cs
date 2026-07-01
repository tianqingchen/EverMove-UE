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
			"AudioCapture",
			"AudioCaptureCore",
			"soma_storage"
		});

		// FAudioCapture discovers its implementation through a modular feature.
		// Link the same concrete backend Unreal selects for each desktop platform
		// so it is registered before any perception actor CDO is constructed.
		if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			PrivateDependencyModuleNames.Add("AudioCaptureRtAudio");
		}
		else if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("AudioCaptureWasapi");
		}

		// ONNX Runtime. Keep the runtime version identical across platforms so
		// inference behavior and the C API surface remain aligned.
		string PluginDirectory = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		string OnnxRuntimeRoot = Path.Combine(PluginDirectory, "ThirdParty", "OnnxRuntime");
		string YoloModelPath = Path.Combine(PluginDirectory, "ThirdParty", "Models", "yolo11n.onnx");
		RuntimeDependencies.Add(YoloModelPath, StagedFileType.NonUFS);

		string OnnxIncludePath = Path.Combine(OnnxRuntimeRoot, "include");
		PublicSystemIncludePaths.Add(OnnxIncludePath);

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			string OnnxLibPath = Path.Combine(OnnxRuntimeRoot, "lib", "Win64");
			string OnnxDllPath = Path.Combine(OnnxLibPath, "onnxruntime.dll");

			PublicAdditionalLibraries.Add(Path.Combine(OnnxLibPath, "onnxruntime.lib"));
			PublicDelayLoadDLLs.Add("onnxruntime.dll");
			RuntimeDependencies.Add(OnnxDllPath);
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			const string OnnxDylibName = "libonnxruntime.1.24.4.dylib";
			string OnnxDylibPath = Path.Combine(OnnxRuntimeRoot, "lib", "Mac", "arm64", OnnxDylibName);

			PublicAdditionalLibraries.Add(OnnxDylibPath);
			PublicDelayLoadDLLs.Add(OnnxDylibPath);
			RuntimeDependencies.Add(Path.Combine("$(TargetOutputDir)", OnnxDylibName), OnnxDylibPath);
		}
		else
		{
			throw new BuildException("soma_perception: ONNX Runtime is not configured for " + Target.Platform);
		}
	}
}

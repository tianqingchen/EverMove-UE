#pragma once

#include "CoreMinimal.h"
#include "SomaPerceptionTypes.h"

#include "onnxruntime_c_api.h"

#include <string>

/**
 * Lightweight ONNX Runtime wrapper for YOLO11n inference.
 * Runs everything on the calling thread (typically the game thread).
 */
class YoloInference
{
public:
	YoloInference();
	~YoloInference();

	// Loads the ONNX model and prepares input/output metadata.
	bool LoadModel(const FString& OnnxPath);

	// Releases the ONNX Runtime session/env.
	void ReleaseSession();

	bool IsLoaded() const { return bLoaded; }

	// RGBPixels is expected as width*height*3 bytes in RGB channel order.
	TArray<FSomaPerceptionDetection> RunInference(
		const TArray<uint8>& RGBPixels,
		int32 Width,
		int32 Height,
		float ConfThresh,
		float NMSThresh);

private:
	static constexpr int32 InputSize = 640;
	static constexpr int32 NumClasses = 80; // COCO

	bool bLoaded = false;

	// ONNX Runtime C API pointers.
	const OrtApi* OrtApi = nullptr;
	OrtEnv* OrtEnv = nullptr;
	OrtSession* OrtSession = nullptr;
	OrtSessionOptions* OrtSessionOptions = nullptr;
	OrtAllocator* OrtAllocator = nullptr;

	// Cached names (UTF-8).
	std::string InputNameUtf8;
	TArray<std::string> OutputNamesUtf8;

	// ONNX Runtime input memory info (CPU).
	OrtMemoryInfo* CpuMemoryInfo = nullptr;

	bool Preprocess(
		const TArray<uint8>& RGBPixels,
		int32 Width,
		int32 Height,
		TArray<float>& OutTensorNCHW) const;

	// Decodes model output and runs per-class NMS.
	TArray<FSomaPerceptionDetection> DecodeAndNms(
		const float* OutputData,
		const TArray<int64_t>& OutputShape,
		int32 OrigWidth,
		int32 OrigHeight,
		float ConfThresh,
		float NMSThresh) const;

	static void GetIou(
		const FSomaPerceptionDetection& A,
		const FSomaPerceptionDetection& B,
		float& OutIou);

	static const TArray<FString>& GetCocoClassNames();
};


#pragma once

#include "CoreMinimal.h"
#include "SomaPerceptionTypes.h"

DECLARE_DELEGATE_FourParams(
	FOnOpenAiVisionResponse,
	TArray<FSomaPerceptionDetection> /*Detections*/,
	FSomaPerceptionSceneSummary /*SceneSummary*/,
	float /*LatencySeconds*/,
	bool /*bSuccess*/);

/**
 * Async HTTP client that sends a viewport capture to the OpenAI Responses API
 * and parses the structured JSON reply into FSomaPerceptionDetection entries.
 */
class FOpenAiVisionClient : public TSharedFromThis<FOpenAiVisionClient>
{
public:
	void SendFrame(
		const TArray<uint8>& RGBPixels,
		int32 Width,
		int32 Height,
		const FString& ApiKey,
		const FString& Model,
		float TimeoutSeconds,
		FOnOpenAiVisionResponse OnComplete);

private:
	static TArray<uint8> EncodeJpeg(const TArray<uint8>& RGBPixels, int32 Width, int32 Height);
	static FString BuildRequestBody(const FString& Base64Image, const FString& Model);
	static TArray<FSomaPerceptionDetection> ParseDetections(const FString& JsonContent, int32 FrameWidth, int32 FrameHeight, FSomaPerceptionSceneSummary& OutSceneSummary);
	static TArray<FSomaPerceptionDetection> ParseDetectionArray(const TArray<TSharedPtr<FJsonValue>>& JsonArray, int32 FrameWidth, int32 FrameHeight);
};

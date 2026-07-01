#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Http.h"
#include "AudioCaptureCore.h"
#include "SomaStorageTypes.h"
#include "SomaPerceptionActor.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FSomaPerceptionOnDetectionComplete,
	const TArray<FSomaPerceptionDetection>&, Detections);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSomaPerceptionOnCaptureEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(
	FSomaPerceptionOnOpenAiComplete,
	const TArray<FSomaPerceptionDetection>&, Detections,
	const FSomaPerceptionSceneSummary&, SceneSummary,
	float, LatencySeconds,
	bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSomaPerceptionOnUserTranscript, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSomaPerceptionOnValidationComplete, bool, bSuccess, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSomaPerceptionOnListeningStarted);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSomaPerceptionOnListeningStopped);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSomaPerceptionOnStatusMessage, const FString&, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSomaPerceptionOnError, const FString&, Error);

class YoloInference;
class FOpenAiVisionClient;
class USomaStorageSubsystem;

DECLARE_LOG_CATEGORY_EXTERN(LogSomaPerception, Log, All);

/**
 * ASomaPerceptionActor
 *
 * Single fat perception actor: vision (YOLO + OpenAI Responses API), microphone
 * push-to-talk + Whisper ASR, and a `SubmitText` API. Broadcasts what the user
 * said via `OnUserTranscript`. Does NOT call any chat / LLM endpoint -- wire
 * `OnUserTranscript` to `ASomaDialogueChatbot::RequestDialogue` for that.
 *
 * Successful OpenAI vision results are forwarded to USomaStorageSubsystem
 * (resolved lazily via GetGameInstance()->GetSubsystem<USomaStorageSubsystem>()).
 * YOLO detections are not currently fed to storage by design.
 */
UCLASS(Blueprintable, meta = (BlueprintSpawnableActor))
class SOMA_PERCEPTION_API ASomaPerceptionActor : public AActor
{
	GENERATED_BODY()

public:
	ASomaPerceptionActor();

	// ── YOLO / Model ──────────────────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|Model")
	FString ModelPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|Inference", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ConfidenceThreshold = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|Inference", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float NMSThreshold = 0.45f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|Capture", meta = (ClampMin = "0.0"))
	float CaptureIntervalSeconds = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|Capture")
	bool bEnableTimerCapture = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|Debug")
	bool bDrawDebugOverlay = true;

	// ── OpenAI ──────────────────────────────────────────────────────

	/** OpenAI API key used by both the vision (Responses API) and Whisper paths. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|OpenAI")
	FString APIKey;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|OpenAI")
	bool bAutoValidateApiKeyOnBeginPlay = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|OpenAI")
	bool bEnableOpenAiVision = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|OpenAI", meta = (ClampMin = "1.0", EditCondition = "bEnableOpenAiVision"))
	float OpenAiIntervalSeconds = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|OpenAI", meta = (EditCondition = "bEnableOpenAiVision"))
	FString OpenAiModel = TEXT("gpt-4o");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|OpenAI", meta = (ClampMin = "1.0", ClampMax = "120.0", EditCondition = "bEnableOpenAiVision"))
	float OpenAiTimeoutSeconds = 30.0f;

	// ── Gaze ────────────────────────────────────────────────────────

	/** When true, periodically collects USomaGazeTargetComponent positions and pushes them to storage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|Gaze")
	bool bEnableGaze = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|Gaze", meta = (ClampMin = "0.1", EditCondition = "bEnableGaze"))
	float GazeIntervalSeconds = 2.0f;

	// ── ASR (Whisper) ───────────────────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|ASR")
	FString WhisperModel = TEXT("whisper-1");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Perception|ASR",
		meta = (GetOptions = "GetAvailableMicrophoneNames"))
	FString InputDeviceName;

	// ── Vision Events ───────────────────────────────────────────────

	UPROPERTY(BlueprintAssignable, Category = "Soma Perception|Events")
	FSomaPerceptionOnDetectionComplete OnDetectionComplete;

	UPROPERTY(BlueprintAssignable, Category = "Soma Perception|Events")
	FSomaPerceptionOnCaptureEvent OnCaptureEvent;

	UPROPERTY(BlueprintAssignable, Category = "Soma Perception|Events")
	FSomaPerceptionOnOpenAiComplete OnOpenAiComplete;

	// ── Audio / Text Events ─────────────────────────────────────────

	UPROPERTY(BlueprintAssignable, Category = "Soma Perception|Events")
	FSomaPerceptionOnUserTranscript OnUserTranscript;

	UPROPERTY(BlueprintAssignable, Category = "Soma Perception|Events")
	FSomaPerceptionOnValidationComplete OnValidationComplete;

	UPROPERTY(BlueprintAssignable, Category = "Soma Perception|Events")
	FSomaPerceptionOnListeningStarted OnListeningStarted;

	UPROPERTY(BlueprintAssignable, Category = "Soma Perception|Events")
	FSomaPerceptionOnListeningStopped OnListeningStopped;

	UPROPERTY(BlueprintAssignable, Category = "Soma Perception|Events")
	FSomaPerceptionOnStatusMessage OnStatusMessage;

	UPROPERTY(BlueprintAssignable, Category = "Soma Perception|Events")
	FSomaPerceptionOnError OnError;

	// ── Vision API ──────────────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "Soma Perception")
	void StartDetection();

	UFUNCTION(BlueprintCallable, Category = "Soma Perception")
	void StopDetection();

	UFUNCTION(BlueprintCallable, Category = "Soma Perception")
	void CaptureAndDetectOnce();

	UFUNCTION(BlueprintCallable, Category = "Soma Perception")
	TArray<FSomaPerceptionDetection> GetLastDetections() const { return LastDetections; }

	UFUNCTION(BlueprintCallable, Category = "Soma Perception")
	FSomaPerceptionSceneSummary GetLastSceneSummary() const { return LastOpenAiSceneSummary; }

	UFUNCTION(BlueprintPure, Category = "Soma Perception")
	bool IsDetectionActive() const { return bDetectionActive; }

	// ── Audio / Text API ────────────────────────────────────────────

	/** Treat `UserText` as if the user typed it: broadcasts OnUserTranscript only (no LLM call, no storage). */
	UFUNCTION(BlueprintCallable, Category = "Soma Perception")
	void SubmitText(const FString& UserText);

	UFUNCTION(BlueprintCallable, Category = "Soma Perception")
	void StartListening();

	UFUNCTION(BlueprintCallable, Category = "Soma Perception")
	void StopListening();

	UFUNCTION(BlueprintCallable, Category = "Soma Perception")
	bool IsListening() const { return bIsListening; }

	UFUNCTION(BlueprintCallable, Category = "Soma Perception")
	void ValidateAPIKey(const FString& Key);

	UFUNCTION(BlueprintCallable, Category = "Soma Perception")
	bool IsApiKeyValidated() const { return bApiKeyValidated; }

	/** Cancels in-flight OpenAI vision and Whisper requests (does not stop the mic; call StopListening for that). */
	UFUNCTION(BlueprintCallable, Category = "Soma Perception")
	void CancelInFlight();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Soma Perception|ASR")
	TArray<FString> GetAvailableMicrophoneNames() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// ── Vision internals ────────────────────────────────────────────
	void EnsureModelLoaded();
	void ReleaseInference();

	bool CaptureViewport(TArray<uint8>& OutRGBPixels, int32& OutWidth, int32& OutHeight) const;
	void DrawDebugOverlay(UCanvas* Canvas, APlayerController* PlayerController);

	void OpenAiTick();
	void OnOpenAiResponse(TArray<FSomaPerceptionDetection> Detections, FSomaPerceptionSceneSummary SceneSummary, float Latency, bool bSuccess);

	// ── Gaze internals ──────────────────────────────────────────────
	void GazeTick();

	// ── Audio internals ─────────────────────────────────────────────
	int32 ResolveCaptureDeviceIndex() const;
	static void AppendPcmWavHeader(TArray<uint8>& WavData, int32 SampleRate, int32 NumChannels, int32 DataSizeBytes);
	void SendWhisperMultipart(const TArray<uint8>& WavFileBytes);
	void OnWhisperTranscriptionResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	void OnModelsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	// ── Storage helpers ─────────────────────────────────────────────
	USomaStorageSubsystem* ResolveStorageSubsystem() const;

	FTimerHandle TimerHandle;
	FTimerHandle OpenAiTimerHandle;
	FTimerHandle GazeTimerHandle;

	bool bDetectionActive = false;
	bool bOpenAiRequestInFlight = false;

	TArray<FSomaPerceptionDetection> LastDetections;
	TArray<FSomaPerceptionDetection> LastOpenAiDetections;
	FSomaPerceptionSceneSummary LastOpenAiSceneSummary;

	int32 LastFrameWidth = 0;
	int32 LastFrameHeight = 0;

	YoloInference* Inference = nullptr;
	TSharedPtr<FOpenAiVisionClient> OpenAiClient;

	FDelegateHandle DebugDrawHandle;

	// Audio capture state.
	TUniquePtr<Audio::FAudioCapture> AudioCapture;
	FCriticalSection CaptureBufferMutex;
	TArray<float> CaptureBuffer;
	bool bIsListening = false;
	/** WASAPI capture callback buffer layout for the active stream (set before StartStream). */
	bool bCaptureDeliversFloat = true;

	bool bApiKeyValidated = false;
	FString PendingValidationKey;

	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveWhisperRequest;
};

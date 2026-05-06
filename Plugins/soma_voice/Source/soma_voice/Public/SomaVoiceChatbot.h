#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Http.h"
#include "Components/AudioComponent.h"
#include "TimerManager.h"
#include "SomaVoiceChatbot.generated.h"

/** Payload for OnVisemeFrame (single USTRUCT param — Blueprint-friendly vs TArray+float TwoParams). */
USTRUCT(BlueprintType)
struct FSomaVisemeFrameData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SomaVoice")
	TArray<float> VisemeScores;

	UPROPERTY(BlueprintReadOnly, Category = "SomaVoice")
	float Timestamp = 0.f;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSomaVoiceOnTextReceived, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSomaVoiceOnValidationComplete, bool, bSuccess, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSomaVoiceOnSpeakingEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSomaVoiceOnStatusMessage, const FString&, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSomaVoiceOnVisemeFrame, FSomaVisemeFrameData, Frame);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FSomaVoiceOnCharacterTiming, const TArray<FString>&, Characters, const TArray<float>&, StartTimes, const TArray<float>&, EndTimes);

DECLARE_LOG_CATEGORY_EXTERN(LogSomaVoice, Log, All);

// --- TTS Provider Enum ---

UENUM(BlueprintType)
enum class ESomaVoiceTTSProvider : uint8
{
	OpenAI,
	ElevenLabs
};

struct FSomaVisemeEntry
{
	float StartTime = 0.f;
	float EndTime = 0.f;
	int32 VisemeIndex = 0;
};

// --- Actor ---

UCLASS()
class SOMA_VOICE_API ASomaVoiceChatbot : public AActor
{
	GENERATED_BODY()

public:
	ASomaVoiceChatbot();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	// ---- Components ----

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UAudioComponent* AudioPlayer;

	// ---- Config ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaVoice|Config")
	ESomaVoiceTTSProvider TTSProvider = ESomaVoiceTTSProvider::OpenAI;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaVoice|Config")
	FString APIKey;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaVoice|Config")
	FString VoiceId = TEXT("alloy");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaVoice|Config")
	bool bAutoValidateApiKeyOnBeginPlay = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaVoice|Config|ElevenLabs")
	FString ElevenLabsAPIKey;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaVoice|Config|ElevenLabs")
	FString ElevenLabsVoiceId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaVoice|Config|ElevenLabs")
	FString ElevenLabsModelId = TEXT("eleven_multilingual_v2");

	// ---- Public API ----

	/** Primary entry point -- send text to the active TTS provider, play audio, emit visemes. */
	UFUNCTION(BlueprintCallable, Category = "SomaVoice")
	void SpeakText(const FString& Text);

	/** Interrupt current playback. */
	UFUNCTION(BlueprintCallable, Category = "SomaVoice")
	void StopSpeaking();

	/** Validate an OpenAI API key (GET /v1/models). */
	UFUNCTION(BlueprintCallable, Category = "SomaVoice")
	void ValidateAPIKey(const FString& Key);

	UFUNCTION(BlueprintCallable, Category = "SomaVoice")
	bool IsApiKeyValidated() const { return bApiKeyValidated; }

	/** Attach audio playback to another actor's root (e.g. MetaHuman head). */
	UFUNCTION(BlueprintCallable, Category = "SomaVoice")
	void SetMetaHumanAttachmentTarget(AActor* Target);

	// ---- Delegates ----

	UPROPERTY(BlueprintAssignable, Category = "SomaVoice|Events")
	FSomaVoiceOnTextReceived OnTextReceived;

	UPROPERTY(BlueprintAssignable, Category = "SomaVoice|Events")
	FSomaVoiceOnValidationComplete OnValidationComplete;

	UPROPERTY(BlueprintAssignable, Category = "SomaVoice|Events")
	FSomaVoiceOnSpeakingEvent OnStartedSpeaking;

	UPROPERTY(BlueprintAssignable, Category = "SomaVoice|Events")
	FSomaVoiceOnSpeakingEvent OnFinishedSpeaking;

	UPROPERTY(BlueprintAssignable, Category = "SomaVoice|Events")
	FSomaVoiceOnStatusMessage OnStatusMessage;

	UPROPERTY(BlueprintAssignable, Category = "SomaVoice|Events")
	FSomaVoiceOnVisemeFrame OnVisemeFrame;

	UPROPERTY(BlueprintAssignable, Category = "SomaVoice|Events")
	FSomaVoiceOnCharacterTiming OnCharacterTiming;

private:
	// ---- OpenAI TTS ----
	void SendTextToOpenAITTS(const FString& Text);
	void OnOpenAITTSResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	// ---- ElevenLabs TTS ----
	void SendTextToElevenLabs(const FString& Text);
	void OnElevenLabsTTSResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	// ---- API key validation ----
	void OnOpenAIModelsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	// ---- Audio playback ----
	void PlayAudioFromWAV(const TArray<uint8>& AudioData);
	void PlayAudioFromPCM(const TArray<uint8>& PCMData, int32 SampleRate, int32 NumChannels);
	void OnTTSPlaybackFinished();
	void ClearTTSFinishedTimer();

	// ---- ElevenLabs viseme timeline ----
	static int32 CharToVisemeIndex(const FString& Char);
	void BuildVisemeTimeline(const TArray<FString>& Chars, const TArray<float>& Starts, const TArray<float>& Ends);
	void TickVisemes();
	void StopVisemeTick();

	// ---- State ----
	bool bApiKeyValidated = false;
	bool bElevenLabsRequestInFlight = false;
	FString PendingValidationKey;
	FTimerHandle TTSFinishedTimerHandle;

	UPROPERTY()
	TObjectPtr<AActor> MetaHumanAttachmentTarget;

	TArray<FSomaVisemeEntry> VisemeTimeline;
	int32 CurrentVisemeIdx = 0;
	FTimerHandle VisemeTickHandle;

	TArray<uint8> LastPCMBuffer;
	int32 LastPCMSampleRate = 0;
	int32 LastPCMNumChannels = 0;
	float PlaybackStartTime = 0.f;
};

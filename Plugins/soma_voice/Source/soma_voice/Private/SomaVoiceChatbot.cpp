#include "SomaVoiceChatbot.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundWaveProcedural.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Base64.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY(LogSomaVoice);

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ASomaVoiceChatbot::ASomaVoiceChatbot()
{
	PrimaryActorTick.bCanEverTick = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	AudioPlayer = CreateDefaultSubobject<UAudioComponent>(TEXT("AudioPlayer"));
	AudioPlayer->SetupAttachment(RootComponent);
	AudioPlayer->bAllowSpatialization = true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ASomaVoiceChatbot::BeginPlay()
{
	Super::BeginPlay();

	if (bAutoValidateApiKeyOnBeginPlay
		&& TTSProvider == ESomaVoiceTTSProvider::OpenAI
		&& !APIKey.IsEmpty()
		&& !bApiKeyValidated)
	{
		ValidateAPIKey(APIKey);
	}
}

void ASomaVoiceChatbot::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearTTSFinishedTimer();
	StopVisemeTick();
	Super::EndPlay(EndPlayReason);
}

// ---------------------------------------------------------------------------
// SetMetaHumanAttachmentTarget
// ---------------------------------------------------------------------------

void ASomaVoiceChatbot::SetMetaHumanAttachmentTarget(AActor* Target)
{
	MetaHumanAttachmentTarget = Target;
	if (AudioPlayer && Target && Target->GetRootComponent())
	{
		AudioPlayer->AttachToComponent(
			Target->GetRootComponent(),
			FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	}
}

// ---------------------------------------------------------------------------
// API Key Validation (OpenAI)
// ---------------------------------------------------------------------------

void ASomaVoiceChatbot::ValidateAPIKey(const FString& Key)
{
	const FString Trimmed = Key.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		OnValidationComplete.Broadcast(false, TEXT("Please enter an API key."));
		return;
	}

	PendingValidationKey = Trimmed;
	OnStatusMessage.Broadcast(TEXT("Validating API key..."));

	FHttpModule& Http = FHttpModule::Get();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http.CreateRequest();
	Request->SetURL(TEXT("https://api.openai.com/v1/models"));
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Trimmed);
	Request->OnProcessRequestComplete().BindUObject(this, &ASomaVoiceChatbot::OnOpenAIModelsResponse);
	Request->ProcessRequest();
}

void ASomaVoiceChatbot::OnOpenAIModelsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	if (!bWasSuccessful || !Response.IsValid())
	{
		PendingValidationKey.Empty();
		OnValidationComplete.Broadcast(false, TEXT("Network error while contacting OpenAI."));
		OnStatusMessage.Broadcast(TEXT("Validation failed (network)."));
		return;
	}

	if (Response->GetResponseCode() != 200)
	{
		PendingValidationKey.Empty();
		const FString Msg = FString::Printf(
			TEXT("HTTP %d: %s"), Response->GetResponseCode(), *Response->GetContentAsString());
		OnValidationComplete.Broadcast(false, Msg);
		OnStatusMessage.Broadcast(TEXT("Invalid API key or access denied."));
		return;
	}

	APIKey = PendingValidationKey;
	PendingValidationKey.Empty();
	bApiKeyValidated = true;
	OnValidationComplete.Broadcast(true, TEXT("API key accepted."));
	OnStatusMessage.Broadcast(TEXT("API key validated."));
}

// ---------------------------------------------------------------------------
// SpeakText -- primary entry point
// ---------------------------------------------------------------------------

void ASomaVoiceChatbot::SpeakText(const FString& Text)
{
	if (Text.IsEmpty())
	{
		return;
	}
	OnTextReceived.Broadcast(Text);

	switch (TTSProvider)
	{
	case ESomaVoiceTTSProvider::OpenAI:
		SendTextToOpenAITTS(Text);
		break;
	case ESomaVoiceTTSProvider::ElevenLabs:
		SendTextToElevenLabs(Text);
		break;
	}
}

// ---------------------------------------------------------------------------
// StopSpeaking
// ---------------------------------------------------------------------------

void ASomaVoiceChatbot::StopSpeaking()
{
	if (AudioPlayer && AudioPlayer->IsPlaying())
	{
		AudioPlayer->Stop();
	}
	ClearTTSFinishedTimer();
	StopVisemeTick();
}

// ---------------------------------------------------------------------------
// OpenAI TTS
// ---------------------------------------------------------------------------

void ASomaVoiceChatbot::SendTextToOpenAITTS(const FString& Text)
{
	if (APIKey.IsEmpty())
	{
		OnStatusMessage.Broadcast(TEXT("OpenAI API key is not set."));
		return;
	}

	OnStatusMessage.Broadcast(TEXT("Generating speech (OpenAI)..."));

	FHttpModule& Http = FHttpModule::Get();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http.CreateRequest();

	Request->SetURL(TEXT("https://api.openai.com/v1/audio/speech"));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + APIKey);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetStringField(TEXT("model"), TEXT("tts-1"));
	Root->SetStringField(TEXT("input"), Text);
	Root->SetStringField(TEXT("voice"), VoiceId);
	Root->SetStringField(TEXT("response_format"), TEXT("wav"));

	FString OutputString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	Request->SetContentAsString(OutputString);
	Request->OnProcessRequestComplete().BindUObject(this, &ASomaVoiceChatbot::OnOpenAITTSResponse);
	Request->ProcessRequest();
}

void ASomaVoiceChatbot::OnOpenAITTSResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	if (!bWasSuccessful || !Response.IsValid() || Response->GetResponseCode() != 200)
	{
		UE_LOG(LogSomaVoice, Error, TEXT("OpenAI TTS request failed."));
		OnStatusMessage.Broadcast(TEXT("TTS request failed."));
		return;
	}

	PlayAudioFromWAV(Response->GetContent());
}

// ---------------------------------------------------------------------------
// ElevenLabs TTS
// ---------------------------------------------------------------------------

void ASomaVoiceChatbot::SendTextToElevenLabs(const FString& Text)
{
	if (ElevenLabsAPIKey.IsEmpty())
	{
		OnStatusMessage.Broadcast(TEXT("ElevenLabs API key is not set."));
		return;
	}
	if (ElevenLabsVoiceId.IsEmpty())
	{
		OnStatusMessage.Broadcast(TEXT("ElevenLabs Voice ID is not set."));
		return;
	}
	if (bElevenLabsRequestInFlight)
	{
		OnStatusMessage.Broadcast(TEXT("ElevenLabs request already in flight; skipping duplicate request."));
		return;
	}
	bElevenLabsRequestInFlight = true;

	OnStatusMessage.Broadcast(TEXT("Generating speech (ElevenLabs)..."));

	const FString URL = FString::Printf(
		TEXT("https://api.elevenlabs.io/v1/text-to-speech/%s/with-timestamps?output_format=pcm_24000"),
		*ElevenLabsVoiceId);

	FHttpModule& Http = FHttpModule::Get();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http.CreateRequest();

	Request->SetURL(URL);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("xi-api-key"), ElevenLabsAPIKey);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetStringField(TEXT("text"), Text);
	Root->SetStringField(TEXT("model_id"), ElevenLabsModelId);

	FString OutputString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	Request->SetContentAsString(OutputString);
	Request->OnProcessRequestComplete().BindUObject(this, &ASomaVoiceChatbot::OnElevenLabsTTSResponse);
	Request->ProcessRequest();
}

void ASomaVoiceChatbot::OnElevenLabsTTSResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	bElevenLabsRequestInFlight = false;
	if (!bWasSuccessful || !Response.IsValid() || Response->GetResponseCode() != 200)
	{
		UE_LOG(LogSomaVoice, Error, TEXT("ElevenLabs TTS request failed (HTTP %d)."),
			Response.IsValid() ? Response->GetResponseCode() : 0);
		OnStatusMessage.Broadcast(TEXT("ElevenLabs TTS request failed."));
		return;
	}

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogSomaVoice, Error, TEXT("Failed to parse ElevenLabs response JSON."));
		OnStatusMessage.Broadcast(TEXT("Failed to parse ElevenLabs response."));
		return;
	}

	FString AudioBase64;
	if (!JsonObject->TryGetStringField(TEXT("audio_base64"), AudioBase64) || AudioBase64.IsEmpty())
	{
		UE_LOG(LogSomaVoice, Error, TEXT("ElevenLabs response missing audio_base64."));
		OnStatusMessage.Broadcast(TEXT("No audio in ElevenLabs response."));
		return;
	}

	TArray<uint8> PCMData;
	FBase64::Decode(AudioBase64, PCMData);

	const bool bHasRiffHeader = PCMData.Num() >= 4
		&& PCMData[0] == static_cast<uint8>('R')
		&& PCMData[1] == static_cast<uint8>('I')
		&& PCMData[2] == static_cast<uint8>('F')
		&& PCMData[3] == static_cast<uint8>('F');
	const bool bHasID3Header = PCMData.Num() >= 3
		&& PCMData[0] == static_cast<uint8>('I')
		&& PCMData[1] == static_cast<uint8>('D')
		&& PCMData[2] == static_cast<uint8>('3');
	const bool bHasOggHeader = PCMData.Num() >= 4
		&& PCMData[0] == static_cast<uint8>('O')
		&& PCMData[1] == static_cast<uint8>('g')
		&& PCMData[2] == static_cast<uint8>('g')
		&& PCMData[3] == static_cast<uint8>('S');
	if (bHasID3Header || bHasOggHeader)
	{
		UE_LOG(LogSomaVoice, Error, TEXT("ElevenLabs returned compressed audio despite requesting pcm_24000. Skipping playback."));
		OnStatusMessage.Broadcast(TEXT("ElevenLabs returned compressed audio. Playback skipped."));
		return;
	}
	if (bHasRiffHeader)
	{
		PlayAudioFromWAV(PCMData);
		return;
	}

	// Parse character-level alignment and broadcast
	const TSharedPtr<FJsonObject>* AlignmentObj = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("alignment"), AlignmentObj) && AlignmentObj)
	{
		TArray<FString> Characters;
		TArray<float> StartTimes;
		TArray<float> EndTimes;

		const TArray<TSharedPtr<FJsonValue>>* CharsArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* StartsArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* EndsArray = nullptr;

		(*AlignmentObj)->TryGetArrayField(TEXT("characters"), CharsArray);
		(*AlignmentObj)->TryGetArrayField(TEXT("character_start_times_seconds"), StartsArray);
		(*AlignmentObj)->TryGetArrayField(TEXT("character_end_times_seconds"), EndsArray);

		if (CharsArray && StartsArray && EndsArray)
		{
			for (const auto& Val : *CharsArray)
			{
				Characters.Add(Val->AsString());
			}
			for (const auto& Val : *StartsArray)
			{
				StartTimes.Add(static_cast<float>(Val->AsNumber()));
			}
			for (const auto& Val : *EndsArray)
			{
				EndTimes.Add(static_cast<float>(Val->AsNumber()));
			}

			OnCharacterTiming.Broadcast(Characters, StartTimes, EndTimes);
			BuildVisemeTimeline(Characters, StartTimes, EndTimes);
		}
	}

	constexpr int32 SampleRate = 24000;
	constexpr int32 NumChannels = 1;
	PlayAudioFromPCM(PCMData, SampleRate, NumChannels);
}

// ---------------------------------------------------------------------------
// Audio Playback (WAV -- OpenAI)
// ---------------------------------------------------------------------------

void ASomaVoiceChatbot::PlayAudioFromWAV(const TArray<uint8>& AudioData)
{
	if (AudioData.Num() < 44)
	{
		return;
	}

	int32 NumChannels = *reinterpret_cast<const int16*>(&AudioData[22]);
	int32 SampleRate = *reinterpret_cast<const int32*>(&AudioData[24]);
	int32 BitsPerSample = *reinterpret_cast<const int16*>(&AudioData[34]);

	if (SampleRate <= 0 || NumChannels <= 0)
	{
		SampleRate = 24000;
		NumChannels = 1;
	}

	const int32 DataSize = AudioData.Num() - 44;
	TArray<uint8> PCMData;
	PCMData.Append(AudioData.GetData() + 44, DataSize);

	LastPCMBuffer = PCMData;
	LastPCMSampleRate = SampleRate;
	LastPCMNumChannels = NumChannels;

	ClearTTSFinishedTimer();
	StopVisemeTick();

	USoundWaveProcedural* SoundWave = NewObject<USoundWaveProcedural>();
	SoundWave->SetSampleRate(SampleRate);
	SoundWave->NumChannels = NumChannels;
	SoundWave->Duration = static_cast<float>(DataSize)
		/ static_cast<float>(SampleRate * NumChannels * (BitsPerSample / 8));

	SoundWave->QueueAudio(PCMData.GetData(), PCMData.Num());

	OnStartedSpeaking.Broadcast();
	OnStatusMessage.Broadcast(TEXT("Playing response..."));
	PlaybackStartTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	AudioPlayer->SetSound(SoundWave);
	AudioPlayer->Play();

	float Duration = SoundWave->Duration;
	if (Duration <= KINDA_SMALL_NUMBER)
	{
		Duration = 0.5f;
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			TTSFinishedTimerHandle,
			FTimerDelegate::CreateUObject(this, &ASomaVoiceChatbot::OnTTSPlaybackFinished),
			Duration,
			false);
	}
}

// ---------------------------------------------------------------------------
// Audio Playback (raw PCM -- ElevenLabs)
// ---------------------------------------------------------------------------

void ASomaVoiceChatbot::PlayAudioFromPCM(const TArray<uint8>& PCMData, int32 SampleRate, int32 NumChannels)
{
	if (PCMData.Num() == 0)
	{
		return;
	}

	constexpr int32 BitsPerSample = 16;
	const int32 SampleByteSize = NumChannels * (BitsPerSample / 8);
	int32 UsableBytes = PCMData.Num();
	if (SampleByteSize > 0 && (UsableBytes % SampleByteSize) != 0)
	{
		UsableBytes -= (UsableBytes % SampleByteSize);
	}
	if (UsableBytes <= 0)
	{
		return;
	}

	TArray<uint8> AlignedPCM;
	if (UsableBytes != PCMData.Num())
	{
		AlignedPCM.Append(PCMData.GetData(), UsableBytes);
	}
	const TArray<uint8>& FinalPCM = (UsableBytes == PCMData.Num()) ? PCMData : AlignedPCM;

	LastPCMBuffer = FinalPCM;
	LastPCMSampleRate = SampleRate;
	LastPCMNumChannels = NumChannels;

	ClearTTSFinishedTimer();
	StopVisemeTick();

	USoundWaveProcedural* SoundWave = NewObject<USoundWaveProcedural>();
	SoundWave->SetSampleRate(SampleRate);
	SoundWave->NumChannels = NumChannels;
	SoundWave->Duration = static_cast<float>(FinalPCM.Num())
		/ static_cast<float>(SampleRate * NumChannels * (BitsPerSample / 8));

	SoundWave->QueueAudio(FinalPCM.GetData(), FinalPCM.Num());

	OnStartedSpeaking.Broadcast();
	OnStatusMessage.Broadcast(TEXT("Playing response..."));
	PlaybackStartTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	AudioPlayer->SetSound(SoundWave);
	AudioPlayer->Play();

	if (UWorld* World = GetWorld())
	{
		CurrentVisemeIdx = 0;
		if (VisemeTimeline.Num() > 0)
		{
			World->GetTimerManager().SetTimer(
				VisemeTickHandle,
				this,
				&ASomaVoiceChatbot::TickVisemes,
				0.033f,
				true);
		}
	}

	float Duration = SoundWave->Duration;
	if (Duration <= KINDA_SMALL_NUMBER)
	{
		Duration = 0.5f;
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			TTSFinishedTimerHandle,
			FTimerDelegate::CreateUObject(this, &ASomaVoiceChatbot::OnTTSPlaybackFinished),
			Duration,
			false);
	}
}

// ---------------------------------------------------------------------------
// Playback completion
// ---------------------------------------------------------------------------

void ASomaVoiceChatbot::ClearTTSFinishedTimer()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(TTSFinishedTimerHandle);
	}
}

void ASomaVoiceChatbot::OnTTSPlaybackFinished()
{
	StopVisemeTick();
	OnFinishedSpeaking.Broadcast();
	OnStatusMessage.Broadcast(TEXT("Finished speaking."));
}

// ---------------------------------------------------------------------------
// ElevenLabs viseme timeline
// ---------------------------------------------------------------------------

int32 ASomaVoiceChatbot::CharToVisemeIndex(const FString& Char)
{
	if (Char.IsEmpty())
	{
		return 0;
	}

	const TCHAR C = FChar::ToLower(Char[0]);

	if (FChar::IsDigit(C) || FChar::IsPunct(C) || FChar::IsWhitespace(C))
	{
		return 0; // sil
	}

	switch (C)
	{
	case TCHAR('p'):
	case TCHAR('b'):
	case TCHAR('m'):
		return 1; // PP
	case TCHAR('f'):
	case TCHAR('v'):
		return 2; // FF
	case TCHAR('t'):
		return 3; // TH (approx)
	case TCHAR('d'):
		return 4; // DD
	case TCHAR('k'):
	case TCHAR('g'):
	case TCHAR('q'):
		return 5; // kk
	case TCHAR('c'):
	case TCHAR('j'):
	case TCHAR('x'):
		return 6; // CH
	case TCHAR('s'):
	case TCHAR('z'):
	case TCHAR('h'):
		return 7; // SS
	case TCHAR('n'):
	case TCHAR('l'):
	case TCHAR('y'):
		return 8; // nn
	case TCHAR('r'):
		return 9; // RR
	case TCHAR('a'):
		return 10; // aa
	case TCHAR('e'):
		return 11; // E
	case TCHAR('i'):
		return 12; // ih
	case TCHAR('o'):
		return 13; // oh
	case TCHAR('u'):
	case TCHAR('w'):
		return 14; // ou
	default:
		return 0; // sil fallback
	}
}

void ASomaVoiceChatbot::BuildVisemeTimeline(const TArray<FString>& Chars, const TArray<float>& Starts, const TArray<float>& Ends)
{
	VisemeTimeline.Reset();
	CurrentVisemeIdx = 0;

	const int32 EntryCount = FMath::Min3(Chars.Num(), Starts.Num(), Ends.Num());
	VisemeTimeline.Reserve(EntryCount);

	for (int32 i = 0; i < EntryCount; ++i)
	{
		const float Start = Starts[i];
		const float End = Ends[i];
		if (End <= Start)
		{
			continue;
		}

		FSomaVisemeEntry Entry;
		Entry.StartTime = Start;
		Entry.EndTime = End;
		Entry.VisemeIndex = CharToVisemeIndex(Chars[i]);
		VisemeTimeline.Add(Entry);
	}
}

void ASomaVoiceChatbot::TickVisemes()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (VisemeTimeline.Num() == 0)
	{
		return;
	}

	const float Elapsed = World->GetTimeSeconds() - PlaybackStartTime;

	while (CurrentVisemeIdx < VisemeTimeline.Num() && VisemeTimeline[CurrentVisemeIdx].EndTime < Elapsed)
	{
		++CurrentVisemeIdx;
	}

	TArray<float> Scores;
	Scores.Init(0.f, 15);

	if (CurrentVisemeIdx < VisemeTimeline.Num())
	{
		const FSomaVisemeEntry& Entry = VisemeTimeline[CurrentVisemeIdx];
		if (Elapsed >= Entry.StartTime && Elapsed <= Entry.EndTime && Scores.IsValidIndex(Entry.VisemeIndex))
		{
			Scores[Entry.VisemeIndex] = 1.f;
		}
	}

	FSomaVisemeFrameData Payload;
	Payload.VisemeScores = Scores;
	Payload.Timestamp = Elapsed;
	OnVisemeFrame.Broadcast(Payload);
}

void ASomaVoiceChatbot::StopVisemeTick()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(VisemeTickHandle);
	}
	CurrentVisemeIdx = 0;
}

#include "SomaPerceptionActor.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/Font.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "UnrealClient.h"
#include "Debug/DebugDrawService.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"
#include "AudioCaptureDeviceInterface.h"

#include "YoloInference.h"
#include "OpenAiVisionClient.h"
#include "SomaStorageSubsystem.h"

DEFINE_LOG_CATEGORY(LogSomaPerception);

ASomaPerceptionActor::ASomaPerceptionActor()
{
	PrimaryActorTick.bCanEverTick = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	ModelPath = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("soma_perception/ThirdParty/Models/yolo11n.onnx"));
}

// ── Lifecycle ─────────────────────────────────────────────────────

void ASomaPerceptionActor::BeginPlay()
{
	Super::BeginPlay();

	if (bDrawDebugOverlay)
	{
		DebugDrawHandle = UDebugDrawService::Register(
			TEXT("SomaPerception"),
			FDebugDrawDelegate::CreateUObject(this, &ASomaPerceptionActor::DrawDebugOverlay));
	}

	if (bAutoValidateApiKeyOnBeginPlay && !APIKey.IsEmpty() && !bApiKeyValidated)
	{
		ValidateAPIKey(APIKey);
	}
}

void ASomaPerceptionActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bIsListening)
	{
		AudioCapture.StopStream();
		AudioCapture.CloseStream();
		bIsListening = false;
		OnListeningStopped.Broadcast();
		{
			FScopeLock Lock(&CaptureBufferMutex);
			CaptureBuffer.Reset();
		}
	}

	CancelInFlight();
	StopDetection();

	if (DebugDrawHandle.IsValid())
	{
		UDebugDrawService::Unregister(DebugDrawHandle);
		DebugDrawHandle.Reset();
	}

	Super::EndPlay(EndPlayReason);
}

// ── Vision ───────────────────────────────────────────────────────

void ASomaPerceptionActor::EnsureModelLoaded()
{
	if (!Inference)
	{
		Inference = new YoloInference();
	}

	if (!Inference->IsLoaded())
	{
		const bool bOk = Inference->LoadModel(ModelPath);
		if (!bOk)
		{
			ReleaseInference();
		}
	}
}

void ASomaPerceptionActor::ReleaseInference()
{
	if (Inference)
	{
		Inference->ReleaseSession();
		delete Inference;
		Inference = nullptr;
	}
}

void ASomaPerceptionActor::StartDetection()
{
	if (bDetectionActive)
	{
		return;
	}

	EnsureModelLoaded();
	if (!Inference || !Inference->IsLoaded())
	{
		UE_LOG(LogSomaPerception, Warning, TEXT("Failed to load model: %s"), *ModelPath);
		return;
	}

	bDetectionActive = true;

	if (bEnableTimerCapture && CaptureIntervalSeconds > 0.0f)
	{
		if (GetWorld())
		{
			GetWorld()->GetTimerManager().SetTimer(
				TimerHandle,
				this,
				&ASomaPerceptionActor::CaptureAndDetectOnce,
				CaptureIntervalSeconds,
				true);
		}
	}
	else
	{
		CaptureAndDetectOnce();
	}

	if (bEnableOpenAiVision && OpenAiIntervalSeconds > 0.0f && GetWorld())
	{
		if (!OpenAiClient.IsValid())
		{
			OpenAiClient = MakeShared<FOpenAiVisionClient>();
		}
		bOpenAiRequestInFlight = false;

		GetWorld()->GetTimerManager().SetTimer(
			OpenAiTimerHandle,
			this,
			&ASomaPerceptionActor::OpenAiTick,
			OpenAiIntervalSeconds,
			true);

		UE_LOG(LogSomaPerception, Log, TEXT("OpenAI cloud vision enabled – interval %.1fs, model %s"),
			OpenAiIntervalSeconds, *OpenAiModel);
	}
}

void ASomaPerceptionActor::StopDetection()
{
	bDetectionActive = false;

	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(TimerHandle);
		GetWorld()->GetTimerManager().ClearTimer(OpenAiTimerHandle);
	}

	bOpenAiRequestInFlight = false;
	OpenAiClient.Reset();

	ReleaseInference();
}

void ASomaPerceptionActor::CaptureAndDetectOnce()
{
	EnsureModelLoaded();
	if (!Inference || !Inference->IsLoaded())
	{
		return;
	}

	TArray<uint8> RGBPixels;
	int32 W = 0;
	int32 H = 0;
	if (!CaptureViewport(RGBPixels, W, H))
	{
		return;
	}

	LastFrameWidth = W;
	LastFrameHeight = H;

	OnCaptureEvent.Broadcast();

	LastDetections = Inference->RunInference(
		RGBPixels,
		W,
		H,
		ConfidenceThreshold,
		NMSThreshold);

	for (FSomaPerceptionDetection& D : LastDetections)
	{
		D.Source = ESomaPerceptionDetectionSource::YOLO;
	}

	OnDetectionComplete.Broadcast(LastDetections);
	// YOLO results are intentionally NOT forwarded to soma_storage in v1.
}

void ASomaPerceptionActor::OpenAiTick()
{
	if (!bDetectionActive || !bEnableOpenAiVision)
	{
		return;
	}

	if (bOpenAiRequestInFlight)
	{
		UE_LOG(LogSomaPerception, Verbose, TEXT("OpenAI request still in flight – skipping tick"));
		return;
	}

	if (APIKey.IsEmpty())
	{
		UE_LOG(LogSomaPerception, Warning, TEXT("OpenAI API key is not set on perception actor"));
		return;
	}

	TArray<uint8> RGBPixels;
	int32 W = 0;
	int32 H = 0;
	if (!CaptureViewport(RGBPixels, W, H))
	{
		return;
	}

	bOpenAiRequestInFlight = true;

	if (!OpenAiClient.IsValid())
	{
		OpenAiClient = MakeShared<FOpenAiVisionClient>();
	}

	FOnOpenAiVisionResponse Callback;
	Callback.BindUObject(this, &ASomaPerceptionActor::OnOpenAiResponse);

	OpenAiClient->SendFrame(
		RGBPixels, W, H,
		APIKey,
		OpenAiModel,
		OpenAiTimeoutSeconds,
		Callback);
}

void ASomaPerceptionActor::OnOpenAiResponse(
	TArray<FSomaPerceptionDetection> Detections, FSomaPerceptionSceneSummary SceneSummary, float Latency, bool bSuccess)
{
	bOpenAiRequestInFlight = false;

	if (bSuccess)
	{
		for (FSomaPerceptionDetection& D : Detections)
		{
			D.LatencySeconds = Latency;
		}
		LastOpenAiDetections = MoveTemp(Detections);
		LastOpenAiSceneSummary = MoveTemp(SceneSummary);
		LastOpenAiSceneSummary.ObjectCount = LastOpenAiDetections.Num();

		UE_LOG(LogSomaPerception, Log, TEXT("OpenAI returned %d detections in %.2fs (summary: %s)"),
			LastOpenAiDetections.Num(), Latency, *LastOpenAiSceneSummary.Summary.Left(120));

		// Forward to soma_storage (vision-only, ASR is logged elsewhere).
		if (USomaStorageSubsystem* Storage = ResolveStorageSubsystem())
		{
			Storage->AddVisionObservation(LastOpenAiSceneSummary, LastOpenAiDetections);
		}
	}
	else
	{
		UE_LOG(LogSomaPerception, Warning, TEXT("OpenAI request failed (%.2fs)"), Latency);
	}

	OnOpenAiComplete.Broadcast(LastOpenAiDetections, LastOpenAiSceneSummary, Latency, bSuccess);
}

bool ASomaPerceptionActor::CaptureViewport(TArray<uint8>& OutRGBPixels, int32& OutWidth, int32& OutHeight) const
{
	if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
	{
		return false;
	}

	FViewport* Viewport = GEngine->GameViewport->Viewport;
	if (!Viewport)
	{
		return false;
	}

	const FIntPoint SizeXY = Viewport->GetSizeXY();
	if (SizeXY.X <= 0 || SizeXY.Y <= 0)
	{
		return false;
	}

	TArray<FColor> SurfacePixels;
	SurfacePixels.SetNumUninitialized(SizeXY.X * SizeXY.Y);

	FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
	const FIntRect ReadRect(0, 0, SizeXY.X, SizeXY.Y);
	const bool bOk = Viewport->ReadPixels(SurfacePixels, ReadFlags, ReadRect);
	if (!bOk)
	{
		return false;
	}

	OutWidth = SizeXY.X;
	OutHeight = SizeXY.Y;

	OutRGBPixels.SetNumUninitialized(SizeXY.X * SizeXY.Y * 3);
	for (int32 i = 0; i < SizeXY.X * SizeXY.Y; ++i)
	{
		const FColor& C = SurfacePixels[i];
		OutRGBPixels[i * 3 + 0] = C.R;
		OutRGBPixels[i * 3 + 1] = C.G;
		OutRGBPixels[i * 3 + 2] = C.B;
	}

	return true;
}

void ASomaPerceptionActor::DrawDebugOverlay(UCanvas* Canvas, APlayerController* PlayerController)
{
	if (!bDrawDebugOverlay || !Canvas)
	{
		return;
	}

	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
	if (!Font)
	{
		return;
	}

	const float CanvasW = (float)Canvas->SizeX;
	const float CanvasH = (float)Canvas->SizeY;

	auto DrawDetections = [&](const TArray<FSomaPerceptionDetection>& Dets, const FLinearColor& BoxColor)
	{
		for (const FSomaPerceptionDetection& Det : Dets)
		{
			const float X0 = FMath::Clamp(Det.BBoxMin.X, 0.0f, CanvasW);
			const float Y0 = FMath::Clamp(Det.BBoxMin.Y, 0.0f, CanvasH);
			const float X1 = FMath::Clamp(Det.BBoxMax.X, 0.0f, CanvasW);
			const float Y1 = FMath::Clamp(Det.BBoxMax.Y, 0.0f, CanvasH);

			const float BoxW = FMath::Max(0.0f, X1 - X0);
			const float BoxH = FMath::Max(0.0f, Y1 - Y0);
			if (BoxW <= 0.0f || BoxH <= 0.0f)
			{
				continue;
			}

			Canvas->K2_DrawBox(
				FVector2D(X0, Y0),
				FVector2D(BoxW, BoxH),
				2.0f,
				BoxColor);

			const FString LabelText = FString::Printf(TEXT("%s %.2f"), *Det.ClassName, Det.Confidence);
			const FVector2D TextPos(X0, FMath::Max(0.0f, Y0 - 14.0f));

			Canvas->K2_DrawText(
				Font,
				LabelText,
				TextPos,
				FVector2D(1.0f, 1.0f),
				FLinearColor::Yellow);

			if (!Det.Description.IsEmpty())
			{
				const FVector2D DescriptionPos(X0, FMath::Min(CanvasH - 14.0f, TextPos.Y + 14.0f));
				Canvas->K2_DrawText(
					Font,
					Det.Description,
					DescriptionPos,
					FVector2D(0.9f, 0.9f),
					FLinearColor::White);
			}
		}
	};

	DrawDetections(LastDetections, FLinearColor::Green);
	DrawDetections(LastOpenAiDetections, FLinearColor(0.0f, 1.0f, 1.0f));
}

// ── Audio / Text input ──────────────────────────────────────────

void ASomaPerceptionActor::SubmitText(const FString& UserText)
{
	const FString Trimmed = UserText.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		OnError.Broadcast(TEXT("SubmitText was called with empty text."));
		return;
	}

	// Per design: SubmitText only broadcasts the transcript event. No LLM call,
	// and no write to dynamic storage (only Vision + ASR are logged).
	OnUserTranscript.Broadcast(Trimmed);
}

void ASomaPerceptionActor::ValidateAPIKey(const FString& Key)
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
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http.CreateRequest();
	Request->SetURL(TEXT("https://api.openai.com/v1/models"));
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Trimmed);
	Request->OnProcessRequestComplete().BindUObject(this, &ASomaPerceptionActor::OnModelsResponse);
	Request->ProcessRequest();
}

void ASomaPerceptionActor::OnModelsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
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

void ASomaPerceptionActor::CancelInFlight()
{
	if (ActiveWhisperRequest.IsValid())
	{
		ActiveWhisperRequest->CancelRequest();
		ActiveWhisperRequest.Reset();
	}
	bOpenAiRequestInFlight = false;
}

TArray<FString> ASomaPerceptionActor::GetAvailableMicrophoneNames() const
{
	TArray<FString> Names;
	Names.Add(TEXT("(System Default)"));

	TArray<Audio::FCaptureDeviceInfo> Devices;
	Audio::FAudioCapture TempCapture;
	if (!TempCapture.GetCaptureDevicesAvailable(Devices))
	{
		return Names;
	}

	for (const Audio::FCaptureDeviceInfo& Info : Devices)
	{
		Names.Add(Info.DeviceName);
	}

	return Names;
}

int32 ASomaPerceptionActor::ResolveCaptureDeviceIndex() const
{
	const FString TrimmedName = InputDeviceName.TrimStartAndEnd();
	if (TrimmedName.IsEmpty() || TrimmedName == TEXT("(System Default)"))
	{
		return INDEX_NONE;
	}

	TArray<Audio::FCaptureDeviceInfo> Devices;
	Audio::FAudioCapture TempCapture;
	if (!TempCapture.GetCaptureDevicesAvailable(Devices))
	{
		return INDEX_NONE;
	}

	for (int32 i = 0; i < Devices.Num(); ++i)
	{
		if (Devices[i].DeviceName == TrimmedName)
		{
			return i;
		}
	}

	return INDEX_NONE;
}

void ASomaPerceptionActor::StartListening()
{
	if (bIsListening)
	{
		return;
	}

	if (APIKey.IsEmpty())
	{
		OnError.Broadcast(TEXT("OpenAI API key is not set."));
		return;
	}

	{
		FScopeLock Lock(&CaptureBufferMutex);
		CaptureBuffer.Reset();
	}

	AudioCapture.CloseStream();

	struct FCaptureAttempt
	{
		Audio::EPCMAudioEncoding Encoding;
		int32 SampleRate;
	};

	// WASAPI shared-mode IAudioClient3::Initialize fails with AUDCLNT_E_UNSUPPORTED_FORMAT (0x88890008) when the
	// format built from enumeration does not match the mix format; try alternate encodings and explicit rates.
	static const FCaptureAttempt CaptureAttempts[] = {
		{ Audio::EPCMAudioEncoding::FLOATING_POINT_32, Audio::InvalidDeviceSampleRate },
		{ Audio::EPCMAudioEncoding::PCM_16, Audio::InvalidDeviceSampleRate },
		{ Audio::EPCMAudioEncoding::FLOATING_POINT_32, 48000 },
		{ Audio::EPCMAudioEncoding::PCM_16, 48000 },
		{ Audio::EPCMAudioEncoding::FLOATING_POINT_32, 44100 },
		{ Audio::EPCMAudioEncoding::PCM_16, 44100 },
		{ Audio::EPCMAudioEncoding::FLOATING_POINT_32, 16000 },
		{ Audio::EPCMAudioEncoding::PCM_16, 16000 },
	};

	const Audio::FOnAudioCaptureFunction OnAudio = [this](const void* InAudio, int32 NumFrames, int32 NumChannels, int32 SampleRate,
		double StreamTime, bool bOverflow)
	{
		if (!InAudio || NumFrames <= 0 || NumChannels <= 0)
		{
			return;
		}

		FScopeLock Lock(&CaptureBufferMutex);
		CaptureBuffer.Reserve(CaptureBuffer.Num() + NumFrames);

		if (bCaptureDeliversFloat)
		{
			const float* AudioData = static_cast<const float*>(InAudio);
			if (NumChannels == 1)
			{
				CaptureBuffer.Append(AudioData, NumFrames);
			}
			else
			{
				for (int32 Frame = 0; Frame < NumFrames; ++Frame)
				{
					float Sum = 0.f;
					for (int32 Ch = 0; Ch < NumChannels; ++Ch)
					{
						Sum += AudioData[Frame * NumChannels + Ch];
					}
					CaptureBuffer.Add(Sum / static_cast<float>(NumChannels));
				}
			}
		}
		else
		{
			const int16* Pcm = static_cast<const int16*>(InAudio);
			for (int32 Frame = 0; Frame < NumFrames; ++Frame)
			{
				float Sum = 0.f;
				for (int32 Ch = 0; Ch < NumChannels; ++Ch)
				{
					Sum += static_cast<float>(Pcm[Frame * NumChannels + Ch]) * (1.f / 32768.f);
				}
				CaptureBuffer.Add(NumChannels == 1 ? Sum : (Sum / static_cast<float>(NumChannels)));
			}
		}

		(void)SampleRate;
		(void)StreamTime;
		(void)bOverflow;
	};

	const int32 ResolvedDeviceIndex = ResolveCaptureDeviceIndex();
	bool bOpened = false;

	for (const FCaptureAttempt& Attempt : CaptureAttempts)
	{
		Audio::FAudioCaptureDeviceParams Params;
		Params.DeviceIndex = ResolvedDeviceIndex;
		Params.NumInputChannels = Audio::InvalidDeviceChannelCount;
		Params.SampleRate = Attempt.SampleRate;
		Params.PCMAudioEncoding = Attempt.Encoding;

		bCaptureDeliversFloat =
			Attempt.Encoding == Audio::EPCMAudioEncoding::FLOATING_POINT_32 ||
			Attempt.Encoding == Audio::EPCMAudioEncoding::FLOATING_POINT_64;

		bOpened = AudioCapture.OpenAudioCaptureStream(Params, OnAudio, 1024u);

		if (bOpened)
		{
			break;
		}

		AudioCapture.CloseStream();
	}

	if (!bOpened)
	{
		OnError.Broadcast(TEXT("Failed to open microphone capture stream (all format attempts failed)."));
		return;
	}

	const bool bStarted = AudioCapture.StartStream();
	if (!bStarted)
	{
		AudioCapture.CloseStream();
		OnError.Broadcast(TEXT("Failed to start microphone capture."));
		return;
	}

	bIsListening = true;
	OnListeningStarted.Broadcast();
	OnStatusMessage.Broadcast(TEXT("Listening..."));
}

void ASomaPerceptionActor::StopListening()
{
	if (!bIsListening)
	{
		return;
	}

	const int32 CapturedSampleRate = AudioCapture.GetSampleRate() > 0 ? static_cast<int32>(AudioCapture.GetSampleRate()) : 16000;

	AudioCapture.StopStream();
	AudioCapture.CloseStream();
	bIsListening = false;
	OnListeningStopped.Broadcast();

	TArray<float> LocalFloats;
	{
		FScopeLock Lock(&CaptureBufferMutex);
		LocalFloats = MoveTemp(CaptureBuffer);
		CaptureBuffer.Reset();
	}

	if (LocalFloats.Num() < 1600)
	{
		OnError.Broadcast(TEXT("Recording too short — no audio sent to Whisper."));
		return;
	}

	const int32 SampleRate = CapturedSampleRate;
	const int32 NumChannels = 1;

	TArray<uint8> PcmData;
	PcmData.AddUninitialized(LocalFloats.Num() * sizeof(int16));
	int16* const Pcm16 = reinterpret_cast<int16*>(PcmData.GetData());
	for (int32 i = 0; i < LocalFloats.Num(); ++i)
	{
		const float Sample = FMath::Clamp(LocalFloats[i], -1.f, 1.f);
		Pcm16[i] = static_cast<int16>(Sample * 32767.0f);
	}

	TArray<uint8> WavBytes;
	AppendPcmWavHeader(WavBytes, SampleRate, NumChannels, PcmData.Num());
	WavBytes.Append(PcmData);

	OnStatusMessage.Broadcast(TEXT("Sending audio to Whisper..."));
	SendWhisperMultipart(WavBytes);
}

void ASomaPerceptionActor::AppendPcmWavHeader(TArray<uint8>& WavData, const int32 SampleRate, const int32 NumChannels, const int32 DataSizeBytes)
{
	const int32 FileSize = 36 + DataSizeBytes;
	const int32 ByteRate = SampleRate * NumChannels * 2;
	const int32 BlockAlign = NumChannels * 2;

	WavData.Empty();
	WavData.AddUninitialized(44);

	uint8* const Header = WavData.GetData();

	FMemory::Memcpy(Header, "RIFF", 4);
	FMemory::Memcpy(Header + 4, &FileSize, 4);
	FMemory::Memcpy(Header + 8, "WAVE", 4);
	FMemory::Memcpy(Header + 12, "fmt ", 4);
	const int32 SubChunk1Size = 16;
	FMemory::Memcpy(Header + 16, &SubChunk1Size, 4);
	const int16 AudioFormat = 1;
	FMemory::Memcpy(Header + 20, &AudioFormat, 2);
	const int16 Channels = static_cast<int16>(NumChannels);
	FMemory::Memcpy(Header + 22, &Channels, 2);
	FMemory::Memcpy(Header + 24, &SampleRate, 4);
	FMemory::Memcpy(Header + 28, &ByteRate, 4);
	const int16 BlockAlign16 = static_cast<int16>(BlockAlign);
	FMemory::Memcpy(Header + 32, &BlockAlign16, 2);
	const int16 BitsPerSample = 16;
	FMemory::Memcpy(Header + 34, &BitsPerSample, 2);
	FMemory::Memcpy(Header + 36, "data", 4);
	FMemory::Memcpy(Header + 40, &DataSizeBytes, 4);
}

void ASomaPerceptionActor::SendWhisperMultipart(const TArray<uint8>& WavFileBytes)
{
	if (ActiveWhisperRequest.IsValid())
	{
		ActiveWhisperRequest->CancelRequest();
		ActiveWhisperRequest.Reset();
	}

	FHttpModule& Http = FHttpModule::Get();
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = Http.CreateRequest();
	ActiveWhisperRequest = HttpRequest;

	HttpRequest->SetURL(TEXT("https://api.openai.com/v1/audio/transcriptions"));
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("Authorization"), FString(TEXT("Bearer ")) + APIKey);

	const FString Boundary = FString(TEXT("---------------------------")) + FGuid::NewGuid().ToString();
	HttpRequest->SetHeader(TEXT("Content-Type"), FString(TEXT("multipart/form-data; boundary=")) + Boundary);

	TArray<uint8> Payload;

	FString ModelPart = TEXT("--") + Boundary + TEXT("\r\n");
	ModelPart += TEXT("Content-Disposition: form-data; name=\"model\"\r\n\r\n");
	ModelPart += WhisperModel + TEXT("\r\n");

	FString FilePart = TEXT("--") + Boundary + TEXT("\r\n");
	FilePart += TEXT("Content-Disposition: form-data; name=\"file\"; filename=\"MicRecording.wav\"\r\n");
	FilePart += TEXT("Content-Type: audio/wav\r\n\r\n");

	const FString EndPart = TEXT("\r\n--") + Boundary + TEXT("--\r\n");

	{
		FTCHARToUTF8 ModelHeader(*ModelPart);
		Payload.Append(reinterpret_cast<const uint8*>(ModelHeader.Get()), ModelHeader.Length());
	}
	{
		FTCHARToUTF8 FileHeader(*FilePart);
		Payload.Append(reinterpret_cast<const uint8*>(FileHeader.Get()), FileHeader.Length());
	}

	Payload.Append(WavFileBytes);

	{
		FTCHARToUTF8 EndFooter(*EndPart);
		Payload.Append(reinterpret_cast<const uint8*>(EndFooter.Get()), EndFooter.Length());
	}

	HttpRequest->SetContent(Payload);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &ASomaPerceptionActor::OnWhisperTranscriptionResponse);
	HttpRequest->ProcessRequest();
}

void ASomaPerceptionActor::OnWhisperTranscriptionResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	if (!ActiveWhisperRequest.IsValid() || ActiveWhisperRequest.Get() != Request.Get())
	{
		return;
	}
	ActiveWhisperRequest.Reset();

	if (!bWasSuccessful || !Response.IsValid())
	{
		OnError.Broadcast(TEXT("Whisper request failed (network)."));
		return;
	}

	if (Response->GetResponseCode() != 200)
	{
		OnError.Broadcast(FString::Printf(TEXT("Whisper HTTP %d: %s"), Response->GetResponseCode(), *Response->GetContentAsString()));
		return;
	}

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		OnError.Broadcast(TEXT("Failed to parse Whisper JSON."));
		return;
	}

	const FString TranscribedText = JsonObject->GetStringField(TEXT("text")).TrimStartAndEnd();
	if (TranscribedText.IsEmpty())
	{
		OnStatusMessage.Broadcast(TEXT("No speech detected in recording."));
		return;
	}

	// Forward to storage (ASR-tagged), then notify listeners. No LLM call -- wire
	// OnUserTranscript to ASomaDialogueChatbot::RequestDialogue in BP for that.
	if (USomaStorageSubsystem* Storage = ResolveStorageSubsystem())
	{
		Storage->AddAsrTranscript(TranscribedText);
	}

	OnUserTranscript.Broadcast(TranscribedText);
}

USomaStorageSubsystem* ASomaPerceptionActor::ResolveStorageSubsystem() const
{
	if (UGameInstance* GI = GetGameInstance())
	{
		return GI->GetSubsystem<USomaStorageSubsystem>();
	}
	return nullptr;
}
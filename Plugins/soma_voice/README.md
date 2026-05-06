# soma_voice Plugin

`soma_voice` is an Unreal Engine plugin that provides text-to-speech playback through OpenAI or ElevenLabs, with runtime delegate events for status, character timing, and viseme frames.

## Overview

- `ASomaVoiceChatbot` is the main actor you place in a level.
- `SpeakText` sends text to the selected provider and plays audio through an internal `UAudioComponent`.
- When using ElevenLabs `/with-timestamps`, the plugin builds a viseme timeline from character alignment and emits `OnVisemeFrame` automatically during playback.
- OpenAI TTS playback works normally, but does not include character timing data, so `OnVisemeFrame` is not emitted for OpenAI.

## Enable the Plugin

1. Copy or keep `soma_voice` under your project's `Plugins` directory.
2. In Unreal Editor, open **Edit > Plugins**.
3. Enable **soma_voice**.
4. Restart the editor if prompted.

## Add and Configure `ASomaVoiceChatbot`

Place an `ASomaVoiceChatbot` actor in your level and configure these properties:

- `TTSProvider`: `OpenAI` or `ElevenLabs`.
- `APIKey`: OpenAI API key (used for OpenAI provider and key validation).
- `VoiceId`: OpenAI voice (default `alloy`).
- `bAutoValidateApiKeyOnBeginPlay`: automatically validates OpenAI key on start.
- `ElevenLabsAPIKey`: ElevenLabs API key.
- `ElevenLabsVoiceId`: ElevenLabs voice ID.
- `ElevenLabsModelId`: ElevenLabs model ID (default `eleven_multilingual_v2`).

Optional:

- Call `SetMetaHumanAttachmentTarget` to attach audio playback to another actor root component.

## Blueprint Usage (Character Key Input Pattern)

Typical setup in a Character Blueprint:

1. Add an `ASomaVoiceChatbot` actor to the level (or spawn one at runtime and keep a reference).
2. In your Character Blueprint, bind an input action (for example key `T`).
3. On key pressed:
   - Build or retrieve the text string.
   - Call `SpeakText(Text)` on the chatbot reference.
4. Optionally call `StopSpeaking()` from another key to interrupt playback.
5. To react to playback or viseme events, bind the chatbot’s **SomaVoice | Events** delegates in the Event Graph (see **Delegate Events** below).

## Delegate Events

`ASomaVoiceChatbot` exposes **Blueprint-assignable** dynamic multicast delegates (category **SomaVoice | Events**). In Blueprint, use **Assign** / **Bind Event to …** on the chatbot reference. In C++, bind with `AddDynamic` and `UFUNCTION` handlers (see below).

- `OnTextReceived`: text has been accepted by `SpeakText`.
- `OnValidationComplete`: OpenAI key validation result.
- `OnStartedSpeaking`: audio playback begins.
- `OnFinishedSpeaking`: playback completes (or timer completes).
- `OnStatusMessage`: status and error text.
- `OnCharacterTiming`: raw ElevenLabs character arrays (`characters`, `start`, `end`).
- `OnVisemeFrame`: one struct `Frame` with `VisemeScores` (15 floats) and `Timestamp`.

### `OnVisemeFrame` behavior

- Fires automatically during ElevenLabs playback.
- Uses an internal character-to-viseme mapping from ElevenLabs timing data.
- No extra third-party lip sync plugin is required.
- Does not fire for OpenAI provider (no character-level alignment is available from that path).

### `OnCharacterTiming` behavior

- Fires for ElevenLabs when `/with-timestamps` alignment is present.
- Provides raw timing arrays so you can implement custom animation or timeline systems.

## C++ Delegate Binding Example

Dynamic multicast delegates require `UFUNCTION` callbacks and `AddDynamic` / `RemoveDynamic` (not `AddLambda`).

```cpp
// In your Actor header (signatures must match the delegate parameters):
UFUNCTION()
void OnSomaStartedSpeaking();

UFUNCTION()
void OnSomaVisemeFrame(const FSomaVisemeFrameData& Frame);

UFUNCTION()
void OnSomaCharacterTiming(
	const TArray<FString>& Characters,
	const TArray<float>& StartTimes,
	const TArray<float>& EndTimes);

// In BeginPlay (after obtaining a valid chatbot pointer):
Chatbot->OnStartedSpeaking.AddDynamic(this, &AMyActor::OnSomaStartedSpeaking);
Chatbot->OnVisemeFrame.AddDynamic(this, &AMyActor::OnSomaVisemeFrame);
Chatbot->OnCharacterTiming.AddDynamic(this, &AMyActor::OnSomaCharacterTiming);

// In EndPlay (or when releasing the chatbot), unregister to avoid dangling calls:
Chatbot->OnStartedSpeaking.RemoveDynamic(this, &AMyActor::OnSomaStartedSpeaking);
Chatbot->OnVisemeFrame.RemoveDynamic(this, &AMyActor::OnSomaVisemeFrame);
Chatbot->OnCharacterTiming.RemoveDynamic(this, &AMyActor::OnSomaCharacterTiming);
```

```cpp
void AMyActor::OnSomaVisemeFrame(const FSomaVisemeFrameData& Frame)
{
	const TArray<float>& VisemeScores = Frame.VisemeScores;
	// VisemeScores has 15 entries (sil, PP, FF, TH, DD, kk, CH, SS, nn, RR, aa, E, ih, oh, ou)
	UE_LOG(LogTemp, Verbose, TEXT("Viseme frame at %.3f sec"), Frame.Timestamp);
}
```

## Notes

- ElevenLabs path requests PCM 24kHz mono output via the `output_format=pcm_24000` **query parameter** on the `/with-timestamps` endpoint, and uses alignment timing to drive visemes.
- OpenAI path uses WAV response playback and does not provide viseme timing data.
- Duplicate ElevenLabs requests are suppressed while one is already in flight to avoid concurrency rate limits.

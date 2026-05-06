# soma_dialogue Plugin

`soma_dialogue` is an Unreal Engine plugin that drives **text-in / text-out** NPC dialogue using OpenAI **Chat Completions** (`/v1/chat/completions`). It does **not** capture audio, run speech-to-text, or play audio. Microphone capture and Whisper ASR live in `soma_perception`; voice playback lives in `soma_voice`. Wire them together in Blueprint.

## Overview

- `ASomaDialogueChatbot` is the main actor you place in a level.
- **Single entry point:** call `RequestDialogue(UserText)` to send a user line to the chat model. The reply is broadcast on `OnDialogueGenerated` once the request completes.
- Rolling history keeps the last `MaxHistoryTurns` user+assistant **pairs** (the system line is always taken from `SystemPrompt`, not from stored history).

The recommended wiring per level is:

```
ASomaPerceptionActor::OnUserTranscript -> ASomaDialogueChatbot::RequestDialogue
ASomaDialogueChatbot::OnDialogueGenerated -> ASomaVoiceChatbot::SpeakText
```

## Enable the Plugin

1. Keep `soma_dialogue` under your project's `Plugins` folder.
2. Ensure `igor.uproject` includes the plugin (see **Project file** below).
3. In Unreal Editor, open **Edit > Plugins**.
4. Enable **Soma Dialogue** (`soma_dialogue`).
5. Restart the editor if prompted.

### Project file

Add this object next to your other `Plugins` entries in `igor.uproject`:

```json
		{
			"Name": "soma_dialogue",
			"Enabled": true
		},
```

## Configure `ASomaDialogueChatbot`

Place an `ASomaDialogueChatbot` actor and set:

| Property | Purpose |
|----------|---------|
| `APIKey` | OpenAI API key. (Note: `soma_perception` keeps its own key for Whisper + vision; consolidating into a shared subsystem is a follow-up.) |
| `bAutoValidateApiKeyOnBeginPlay` | If true and `APIKey` is set, runs `ValidateAPIKey` on `BeginPlay`. |
| `ChatModel` | Chat Completions model (default `gpt-4o-mini`). |
| `SystemPrompt` | First `system` message on every request. |
| `MaxHistoryTurns` | Max stored user+assistant **pairs** (default `10`). |

## Blueprint wiring (recommended)

1. Place references to **`ASomaPerceptionActor`**, **`ASomaDialogueChatbot`**, and **`ASomaVoiceChatbot`** (or spawn at runtime).
2. **Push-to-talk:** on input pressed → call `StartListening` on the perception actor; on released → `StopListening`. Whisper transcription completes on perception, which broadcasts `OnUserTranscript`.
3. **Text input:** on commit → call `SubmitText` on the perception actor with the typed string. It broadcasts `OnUserTranscript` (no LLM yet).
4. **Bind `OnUserTranscript` -> `RequestDialogue`** on the dialogue actor. The dialogue actor handles Chat Completions only.
5. **Voice output:** bind **`OnDialogueGenerated`** on the dialogue actor to **`SpeakText`** on the voice actor.
6. Optionally bind **`OnStatusMessage`**, **`OnError`**, and **`OnValidationComplete`** for UI / logging.

## C++: bind `OnDialogueGenerated` to `SpeakText`

Dynamic multicast delegates need `UFUNCTION` handlers and `AddDynamic`:

```cpp
// In your actor header:
UPROPERTY()
TObjectPtr<ASomaDialogueChatbot> DialogueBot;

UPROPERTY()
TObjectPtr<ASomaVoiceChatbot> VoiceBot;

UFUNCTION()
void HandleDialogueGenerated(const FString& Text);

// In cpp (e.g. BeginPlay):
if (DialogueBot && VoiceBot)
{
	DialogueBot->OnDialogueGenerated.AddDynamic(this, &AMyActor::HandleDialogueGenerated);
}

void AMyActor::HandleDialogueGenerated(const FString& Text)
{
	if (VoiceBot)
	{
		VoiceBot->SpeakText(Text);
	}
}
```

## Utility functions

- **`RequestDialogue(UserText)`** — primary entry point, replaces the old `SubmitText`. Sends `UserText` to the chat model and broadcasts the reply on `OnDialogueGenerated`.
- **`ResetConversation`** — clears stored user/assistant history.
- **`CancelInFlight`** — cancels the pending HTTP chat request. (Mic and Whisper requests are owned by `soma_perception`.)

## Migration notes (from the previous all-in-one dialogue plugin)

- `SubmitText` is renamed to `RequestDialogue`. The old `SubmitText` did two things — broadcast the user line then send to chat. The transcript broadcast moved to `ASomaPerceptionActor::SubmitText` (which only emits `OnUserTranscript`). The chat call moved to `RequestDialogue`. Wire them together in BP.
- `StartListening`, `StopListening`, `IsListening`, `GetAvailableMicrophoneNames`, `WhisperModel`, and `InputDeviceName` are removed; they live on `ASomaPerceptionActor` now.
- `OnUserTranscript`, `OnListeningStarted`, and `OnListeningStopped` delegates moved to `ASomaPerceptionActor` (renamed with `FSomaPerception...` prefix).
- The `AudioCaptureCore` dependency is dropped from `soma_dialogue.Build.cs`.

## Notes

- The legacy `ALLMChatbot` in game module `igor` is **unchanged**; this plugin is standalone.
- Ensure **HTTPS** / network access to `api.openai.com` from your target platform.

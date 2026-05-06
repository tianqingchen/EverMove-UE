# soma_perception Plugin

`soma_perception` is the perception layer for the Soma stack. A single
`ASomaPerceptionActor` covers:

- **Vision** — periodic viewport capture sent to a local YOLO11n ONNX model and
  optionally to the OpenAI Responses API (`/v1/responses`) for richer scene
  descriptions.
- **ASR** — push-to-talk microphone capture via `Audio::FAudioCapture`, posted
  to OpenAI Whisper (`/v1/audio/transcriptions`) for transcription.
- **Text input** — `SubmitText` for typed lines.

It intentionally does **no** LLM dialogue work. Successful OpenAI vision
results and Whisper transcripts are forwarded to `USomaStorageSubsystem`
(in `soma_storage`); user input (mic or typed) is broadcast on
`OnUserTranscript`. Wire that to `ASomaDialogueChatbot::RequestDialogue` for
chat replies.

> Renamed from `soma_vision`. `FSomaDetection` / `FSomaSceneSummary` are now
> `FSomaPerceptionDetection` / `FSomaPerceptionSceneSummary` and live in
> `soma_storage` (re-exported via `SomaPerceptionTypes.h`). The actor is
> `ASomaPerceptionActor` (was `ASomaVisionDetector`). Existing Blueprint
> nodes that referenced the old names will need to be reconnected.

## Enable the plugin

1. In Unreal Editor, open **Edit > Plugins**.
2. Enable **Soma Perception**.
3. Restart the editor if prompted.

`soma_perception` depends on `soma_storage`, so make sure that plugin is
enabled too (it is in `igor.uproject`).

## Export YOLO11n (`.pt`) to ONNX (`.onnx`)

Prerequisite: Python with `ultralytics` installed.

```bash
pip install ultralytics
```

Export (640x640, simplified ONNX):

```bash
yolo export model=yolo11n.pt format=onnx imgsz=640 simplify=True opset=13
```

This produces `yolo11n.onnx` (size will vary, typically around ~10-15MB).
Copy the exported model to:

`Plugins/soma_perception/ThirdParty/Models/yolo11n.onnx`

## Install ONNX Runtime (CPU, Windows x64)

1. Download the **ONNX Runtime 1.17.x CPU** package for **Windows x64** from the official releases:
   [https://github.com/microsoft/onnxruntime/releases](https://github.com/microsoft/onnxruntime/releases)
2. Unzip the archive.
3. Copy these files into your plugin:

Headers:
`Plugins/soma_perception/ThirdParty/OnnxRuntime/include/`

Library:
`Plugins/soma_perception/ThirdParty/OnnxRuntime/lib/Win64/onnxruntime.lib`

Runtime DLL:
`Plugins/soma_perception/ThirdParty/OnnxRuntime/lib/Win64/onnxruntime.dll`

## Configure `ASomaPerceptionActor`

Place an `ASomaPerceptionActor` in your level. Key properties:

| Category | Property | Purpose |
|----------|----------|---------|
| Model | `ModelPath` | Path to YOLO ONNX model. Defaults to `Plugins/soma_perception/ThirdParty/Models/yolo11n.onnx`. |
| Inference | `ConfidenceThreshold`, `NMSThreshold` | YOLO post-processing thresholds. |
| Capture | `CaptureIntervalSeconds`, `bEnableTimerCapture` | Periodic YOLO capture. |
| Debug | `bDrawDebugOverlay` | Draws bounding boxes via `UDebugDrawService`. |
| OpenAI | `APIKey`, `bAutoValidateApiKeyOnBeginPlay` | Used for both the vision (Responses API) and Whisper paths. |
| OpenAI | `bEnableOpenAiVision`, `OpenAiIntervalSeconds`, `OpenAiModel`, `OpenAiTimeoutSeconds` | Periodic cloud vision call. |
| ASR | `WhisperModel`, `InputDeviceName` | `(System Default)` is the first option in the mic dropdown. |

## Blueprint API

### Vision

- `StartDetection()` / `StopDetection()` — start/stop YOLO timer + OpenAI timer.
- `CaptureAndDetectOnce()` — single on-demand YOLO capture + inference.
- `GetLastDetections()` / `GetLastSceneSummary()` — last results from each path.
- Delegates: `OnDetectionComplete`, `OnCaptureEvent`, `OnOpenAiComplete`.

### ASR / text

- `StartListening()` / `StopListening()` / `IsListening()` — push-to-talk mic capture. On stop, the WAV is built in memory and posted to Whisper.
- `SubmitText(UserText)` — broadcasts `OnUserTranscript` only. Does **not** call any LLM.
- `ValidateAPIKey(Key)` / `IsApiKeyValidated()` — check the key against `/v1/models`.
- `CancelInFlight()` — cancels pending Whisper / vision requests (does not stop the mic; call `StopListening` for that).
- `GetAvailableMicrophoneNames()` — populates the `InputDeviceName` dropdown. First entry is `(System Default)`.
- Delegates: `OnUserTranscript`, `OnListeningStarted`, `OnListeningStopped`, `OnStatusMessage`, `OnError`, `OnValidationComplete`.

### Storage hooks

- On a successful OpenAI Responses API call, perception calls
  `USomaStorageSubsystem::AddVisionObservation` with the parsed
  `FSomaPerceptionSceneSummary` and `TArray<FSomaPerceptionDetection>`.
- On a successful Whisper response, perception calls
  `USomaStorageSubsystem::AddAsrTranscript(Text)` and then broadcasts
  `OnUserTranscript`.
- The subsystem is resolved lazily via
  `GetGameInstance()->GetSubsystem<USomaStorageSubsystem>()`. Perception
  still works without `soma_storage` enabled — the storage calls just become
  no-ops.

YOLO detections are **not yet feeding storage**; only the OpenAI vision path
writes to the rolling perception log in v1. Adding YOLO is straightforward
once the schema converges.

## Recommended wiring

```
[input pressed]    -> ASomaPerceptionActor::StartListening
[input released]   -> ASomaPerceptionActor::StopListening
[ui submit]        -> ASomaPerceptionActor::SubmitText(InputBuffer)

ASomaPerceptionActor::OnUserTranscript -> ASomaDialogueChatbot::RequestDialogue
ASomaDialogueChatbot::OnDialogueGenerated -> ASomaVoiceChatbot::SpeakText
```

Each plugin owns one concern: perception captures input and observations,
storage holds them, dialogue runs the LLM, voice plays audio.

## Cost & latency guidance for OpenAI vision

| Parameter | Typical value | Note |
|---|---|---|
| Interval | 5–15 s | Lower = more API calls and higher cost |
| Latency per call | 2–8 s | Varies by model, image size, network |
| Cost per call | ~$0.01–0.05 | Depends on model and token usage |

The richer JSON schema (`scene_summary`, per-object `description`,
`attributes`, `action`) increases output tokens versus a minimal
bounding-box response. Start with `OpenAiIntervalSeconds = 10` during
development.

## Debug overlay

YOLO detections are drawn in **green**; OpenAI detections in **cyan**. Both
are visible simultaneously when `bDrawDebugOverlay` is enabled. For OpenAI
detections, the overlay draws `Description` as a second line under the
class/confidence label.

## Notes / Limitations

- CPU-only inference for YOLO (no CUDA / DirectML).
- YOLO inference runs on the calling thread (game thread).
- Frames are resized to `640x640` by simple stretching before inference (no letterboxing).
- The OpenAI HTTP requests run asynchronously and do not block the game thread.
- API keys are kept per plugin (perception + dialogue each have their own
  `APIKey`). Consolidating into a shared subsystem is a deliberate follow-up.

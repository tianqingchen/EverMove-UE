# soma_storage Plugin

`soma_storage` holds the shared perception data for the Soma stack:

- A **rolling dynamic log** of recent perception entries (vision observations
  + ASR transcripts), with oldest-first eviction by word budget.
- A **persistent character text file** that is auto-reloaded via
  `IDirectoryWatcher` when something else (a tool, a teammate, you in your
  editor of choice) updates it on disk.

There is **no HTTP** and **no LLM** in this plugin. `soma_perception` writes
to it; `soma_dialogue` will read from it later when storage injection into the
chat prompt is enabled (see "Future" in the plan).

## Enable the plugin

1. Place under `Plugins/soma_storage`.
2. Make sure `igor.uproject` lists it:

```json
		{
			"Name": "soma_storage",
			"Enabled": true
		},
```

3. In Unreal Editor, open **Edit > Plugins** and enable **Soma Storage**.

## What lives where

- `SomaStorageTypes.h` defines the shared perception data shapes
  (`FSomaPerceptionDetection`, `ESomaPerceptionDetectionSource`,
  `FSomaPerceptionSceneSummary`) plus the storage entry types
  (`FSomaPerceptionEntry`, `ESomaPerceptionEntrySource`) and the storage
  delegates. The perception types live here so `USomaStorageSubsystem` can
  accept them directly without forcing a circular dependency on
  `soma_perception`.
- `USomaStorageSubsystem` is a `UGameInstanceSubsystem`. Resolve it from any
  actor with:

```cpp
USomaStorageSubsystem* Storage = GetGameInstance()->GetSubsystem<USomaStorageSubsystem>();
```

## Configuration (`USomaStorageSubsystem`)

| Property | Default | Notes |
|----------|---------|-------|
| `MaxWords` | `1000` | Soft cap on total whitespace-separated words across all entries. Oldest entries are evicted first. |
| `MaxEntries` | `200` | Hard cap on the number of entries kept (safety valve). |
| `PersistentTextPath` | `<ProjectSaved>/SomaStorage/character.txt` | Absolute path to the persistent text file. The file and its directory are created on first run if missing. |

These are `UPROPERTY(Config)` so you can override them in `DefaultGame.ini`
under `[/Script/soma_storage.SomaStorageSubsystem]` if needed.

## Dynamic log API

| Function | Description |
|----------|-------------|
| `AddVisionObservation(Summary, Detections)` | Renders a vision observation into a single text block (timestamp + scene summary + bullet list of `Description`/`ClassName`/`Action`/`Attributes`, no bbox to keep word budget low) and appends it as a `Vision` entry. |
| `AddAsrTranscript(Transcript)` | Appends a Whisper transcript as a single `ASR` entry. Empty / whitespace-only transcripts are ignored. |
| `BuildDynamicContext()` | Oldest-first concatenation of all entries separated by `---`. This is the LLM-ready string to splice into a chat prompt. |
| `GetEntries()` | Copy of all retained `FSomaPerceptionEntry` values. |
| `GetTotalWords()` | Cached total whitespace-separated word count across all entries. |
| `ClearDynamicLog()` | Drops every entry. |
| `CompressOldestWindow()` | Empty hook in v1. Override (or extend in a follow-up) to summarize the oldest entries via an LLM without touching ingest. |

Eviction runs after every `Add*` call: while `Entries.Num() > MaxEntries` or
`TotalWords > MaxWords`, the oldest entry is removed and `TotalWords` is
adjusted. Each entry caches its `WordCount` at append time so eviction is
O(N) on the entry array, not on text length.

## Persistent text API

| Function / Delegate | Description |
|---------------------|-------------|
| `GetPersistentText()` | Returns the text most recently loaded from `PersistentTextPath`. |
| `ReloadPersistentText()` | Forces a reload from disk. |
| `OnPersistentTextChanged(NewText)` | Broadcast whenever the file's contents change (initial load, `ReloadPersistentText`, or directory watcher). |

`Initialize` ensures the directory and file exist, performs the initial load,
and registers a `IDirectoryWatcher` callback on the parent directory. When a
change for `PersistentTextPath` lands, the file is reloaded and
`OnPersistentTextChanged` fires only if the contents actually changed.

`Deinitialize` unregisters the watcher.

## Delegates

| Delegate | When |
|----------|------|
| `OnDynamicLogChanged` | After every successful `AddVisionObservation` / `AddAsrTranscript` (and after `ClearDynamicLog`). |
| `OnPersistentTextChanged` | After the persistent text file changes on disk (or after a manual `ReloadPersistentText` if the contents differ). |

## Example: dump dynamic context to log

```cpp
if (USomaStorageSubsystem* Storage = GetGameInstance()->GetSubsystem<USomaStorageSubsystem>())
{
    UE_LOG(LogTemp, Log, TEXT("Dynamic context:\n%s"), *Storage->BuildDynamicContext());
}
```

## Notes

- v1 uses whole-entry oldest-first eviction by `MaxWords` only — there is no
  per-entry truncation or summarization. `CompressOldestWindow()` is the
  override point for adding that later.
- `SubmitText` on the perception actor is **not** logged (only Vision + ASR
  are). Typed input goes straight to the dialogue LLM via
  `OnUserTranscript -> RequestDialogue`.
- YOLO detections are not yet routed to storage; only the OpenAI Responses
  API path writes to the dynamic log.

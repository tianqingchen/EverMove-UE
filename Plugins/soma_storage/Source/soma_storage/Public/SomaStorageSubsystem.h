#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SomaStorageTypes.h"
#include "SomaStorageSubsystem.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSomaStorage, Log, All);

/**
 * USomaStorageSubsystem
 *
 * Holds the dynamic perception log (rolling, oldest-first eviction by word count)
 * plus a persistent character text file that is hot-reloaded via IDirectoryWatcher.
 *
 * No HTTP. No LLM. soma_perception writes to it; soma_dialogue can read from it
 * later to build prompts (see "Future" notes in the plan).
 */
UCLASS(Config = Game, defaultconfig)
class SOMA_STORAGE_API USomaStorageSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// ---- UGameInstanceSubsystem ----
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ---- Config ----

	/** Soft cap on total whitespace-separated words across all entries. Oldest entries are evicted first. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "SomaStorage|Dynamic")
	int32 MaxWords = 1000;

	/** Hard cap on the number of entries kept. Acts as a safety valve in case word counting drifts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "SomaStorage|Dynamic")
	int32 MaxEntries = 200;

	/** When false, AddVisionObservation and AddAsrTranscript silently discard incoming data. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "SomaStorage|Dynamic")
	bool bAutoIngestEnabled = true;

	/** Absolute path to the dynamic-log text file. Defaults to <Saved>/SomaStorage/dynamic_log.txt. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "SomaStorage|Dynamic")
	FString DynamicLogFilePath;

	/** Absolute path to the persistent character text file. Defaults to <Saved>/SomaStorage/character.txt. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "SomaStorage|Persistent")
	FString PersistentTextPath;

	/** Absolute path to the prompt_config.json file. Defaults to <Saved>/SomaStorage/prompt_config.json. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "SomaStorage|Prompt")
	FString PromptConfigPath;

	/** Cached config loaded from prompt_config.json. */
	UPROPERTY(BlueprintReadOnly, Category = "SomaStorage|Prompt")
	FSomaPromptConfig PromptConfig;

	// ---- Dynamic log API ----

	/** Renders a vision observation into a single text block, appends it as an entry, and evicts oldest entries while over budget. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Dynamic")
	void AddVisionObservation(const FSomaPerceptionSceneSummary& Summary, const TArray<FSomaPerceptionDetection>& Detections);

	/** Appends a Whisper transcript as a single ASR entry. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Dynamic")
	void AddAsrTranscript(const FString& Transcript);

	/** Injects an arbitrary text entry into the dynamic log. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Dynamic")
	void AppendDynamicEntry(const FString& Text, const FString& SourceTag = TEXT("Custom"));

	/** Toggles whether AddVisionObservation / AddAsrTranscript actually append. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Dynamic")
	void SetAutoIngestEnabled(bool bEnabled);

	/** Returns the LLM-ready dynamic context: oldest-first concatenation of all entry blocks separated by `---`. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Dynamic")
	FString BuildDynamicContext() const;

	/** Returns a copy of the entries currently in the dynamic log (oldest-first). */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Dynamic")
	TArray<FSomaPerceptionEntry> GetEntries() const { return Entries; }

	/** Total whitespace-separated words across all entries currently retained. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Dynamic")
	int32 GetTotalWords() const { return TotalWords; }

	/** Clears the dynamic log. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Dynamic")
	void ClearDynamicLog();

	// ---- Gaze API ----

	/**
	 * Replaces the live gaze-candidate snapshot. Always refreshes the in-memory cache
	 * (used for prompt building + position resolution); additionally appends a `[GAZE]`
	 * block to the dynamic log, but only when the candidate set / positions change
	 * meaningfully so the rolling word-budget log is not flooded every tick.
	 */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Gaze")
	void UpdateGazeCandidates(const TArray<FSomaGazeCandidate>& Candidates);

	/** Resolves a gaze object name (case-insensitive) to its live world position. Returns false if not found. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Gaze")
	bool ResolveGazePosition(const FString& Name, FVector& OutPosition) const;

	/** Returns a copy of the current live gaze candidates. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Gaze")
	TArray<FSomaGazeCandidate> GetGazeCandidates() const { return GazeCandidates; }

	/**
	 * Hook for a future LLM-based oldest-window summarization pass.
	 * v1: empty / never called. Subclasses (or a follow-up patch) can override to
	 * collapse the oldest entries into a single summarized entry without touching ingest.
	 */
	virtual void CompressOldestWindow() {}

	// ---- Structured prompt API ----

	/** Assembles the full system prompt from PromptConfig + dynamic log + output format instructions. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Prompt")
	FString BuildStructuredSystemPrompt() const;

	/** Parses a JSON response from the LLM into a structured response. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Prompt")
	static FSomaStructuredResponse ParseStructuredResponse(const FString& JsonString);

	/** Reloads prompt_config.json from disk. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Prompt")
	void ReloadPromptConfig();

	// ---- Persistent text API ----

	/** Returns the most recently loaded persistent text. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Persistent")
	FString GetPersistentText() const { return PersistentText; }

	/** Force a reload of the persistent text file from disk. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Persistent")
	void ReloadPersistentText();

	// ---- Delegates ----

	UPROPERTY(BlueprintAssignable, Category = "SomaStorage|Events")
	FSomaStorageOnDynamicLogChanged OnDynamicLogChanged;

	UPROPERTY(BlueprintAssignable, Category = "SomaStorage|Events")
	FSomaStorageOnPersistentChanged OnPersistentTextChanged;

private:
	void EvictWhileOverBudget();
	static int32 CountWords(const FString& Text);
	static FString RenderVisionBlock(const FDateTime& When, const FSomaPerceptionSceneSummary& Summary, const TArray<FSomaPerceptionDetection>& Detections);
	static FString RenderAsrBlock(const FDateTime& When, const FString& Transcript);
	static FString RenderGazeBlock(const FDateTime& When, const TArray<FSomaGazeCandidate>& Candidates);

	void SaveDynamicLogToFile();
	void LoadDynamicLogFromFile();

	void RegisterDirectoryWatcher();
	void UnregisterDirectoryWatcher();
#if WITH_EDITOR
	void OnPersistentDirectoryChanged(const TArray<struct FFileChangeData>& FileChanges);
#else
	bool TickFileWatcher(float DeltaTime);
#endif
	void EnsurePersistentFileExists();
	void EnsurePromptConfigFileExists();

	UPROPERTY(Transient)
	TArray<FSomaPerceptionEntry> Entries;

	/** Live gaze-candidate snapshot, refreshed every collector tick. */
	UPROPERTY(Transient)
	TArray<FSomaGazeCandidate> GazeCandidates;

	int32 TotalWords = 0;

	/** Signature of the last gaze set appended to the dynamic log; used to suppress redundant `[GAZE]` blocks. */
	FString LastLoggedGazeSignature;

	FString PersistentText;

#if WITH_EDITOR
	FString WatchedDirectory;
	FDelegateHandle DirectoryWatcherHandle;
#else
	FTSTicker::FDelegateHandle FileWatcherTickerHandle;
	FDateTime PersistentTextTimestamp;
	FDateTime PromptConfigTimestamp;
#endif
};

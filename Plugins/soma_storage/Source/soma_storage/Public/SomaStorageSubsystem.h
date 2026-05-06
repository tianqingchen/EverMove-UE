#pragma once

#include "CoreMinimal.h"
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

	/** Absolute path to the persistent character text file. Defaults to <Saved>/SomaStorage/character.txt. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "SomaStorage|Persistent")
	FString PersistentTextPath;

	// ---- Dynamic log API ----

	/** Renders a vision observation into a single text block, appends it as an entry, and evicts oldest entries while over budget. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Dynamic")
	void AddVisionObservation(const FSomaPerceptionSceneSummary& Summary, const TArray<FSomaPerceptionDetection>& Detections);

	/** Appends a Whisper transcript as a single ASR entry. */
	UFUNCTION(BlueprintCallable, Category = "SomaStorage|Dynamic")
	void AddAsrTranscript(const FString& Transcript);

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

	/**
	 * Hook for a future LLM-based oldest-window summarization pass.
	 * v1: empty / never called. Subclasses (or a follow-up patch) can override to
	 * collapse the oldest entries into a single summarized entry without touching ingest.
	 */
	virtual void CompressOldestWindow() {}

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

	void RegisterDirectoryWatcher();
	void UnregisterDirectoryWatcher();
	void OnPersistentDirectoryChanged(const TArray<struct FFileChangeData>& FileChanges);
	void EnsurePersistentFileExists();

	UPROPERTY(Transient)
	TArray<FSomaPerceptionEntry> Entries;

	int32 TotalWords = 0;

	FString PersistentText;

	FString WatchedDirectory;
	FDelegateHandle DirectoryWatcherHandle;
};

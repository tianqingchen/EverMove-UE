#include "SomaStorageSubsystem.h"

#if WITH_EDITOR
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#endif
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

DEFINE_LOG_CATEGORY(LogSomaStorage);

namespace SomaStoragePrivate
{
	static const TCHAR* const EntrySeparator = TEXT("\n---\n");
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void USomaStorageSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (PersistentTextPath.IsEmpty())
	{
		PersistentTextPath = FPaths::ProjectSavedDir() / TEXT("SomaStorage/character.txt");
	}
	if (DynamicLogFilePath.IsEmpty())
	{
		DynamicLogFilePath = FPaths::ProjectSavedDir() / TEXT("SomaStorage/dynamic_log.txt");
	}
	if (PromptConfigPath.IsEmpty())
	{
		PromptConfigPath = FPaths::ProjectSavedDir() / TEXT("SomaStorage/prompt_config.json");
	}

	EnsurePersistentFileExists();
	EnsurePromptConfigFileExists();
	ReloadPersistentText();
	ReloadPromptConfig();
	RegisterDirectoryWatcher();
	LoadDynamicLogFromFile();

	UE_LOG(LogSomaStorage, Log,
		TEXT("SomaStorageSubsystem initialized. PersistentTextPath=%s, DynamicLogFilePath=%s, PromptConfigPath=%s, MaxWords=%d, MaxEntries=%d"),
		*PersistentTextPath, *DynamicLogFilePath, *PromptConfigPath, MaxWords, MaxEntries);
}

void USomaStorageSubsystem::Deinitialize()
{
	SaveDynamicLogToFile();
	UnregisterDirectoryWatcher();

	Entries.Reset();
	TotalWords = 0;
	PersistentText.Reset();

	Super::Deinitialize();
}

// ---------------------------------------------------------------------------
// Dynamic log
// ---------------------------------------------------------------------------

void USomaStorageSubsystem::AddVisionObservation(
	const FSomaPerceptionSceneSummary& Summary,
	const TArray<FSomaPerceptionDetection>& Detections)
{
	if (!bAutoIngestEnabled)
	{
		return;
	}

	const FDateTime Now = FDateTime::UtcNow();

	FSomaPerceptionEntry Entry;
	Entry.Timestamp = Now;
	Entry.Source = ESomaPerceptionEntrySource::Vision;
	Entry.Text = RenderVisionBlock(Now, Summary, Detections);
	Entry.WordCount = CountWords(Entry.Text);

	TotalWords += Entry.WordCount;
	Entries.Add(MoveTemp(Entry));

	EvictWhileOverBudget();
	OnDynamicLogChanged.Broadcast();
	SaveDynamicLogToFile();
}

void USomaStorageSubsystem::AddAsrTranscript(const FString& Transcript)
{
	if (!bAutoIngestEnabled)
	{
		return;
	}

	const FString Trimmed = Transcript.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return;
	}

	const FDateTime Now = FDateTime::UtcNow();

	FSomaPerceptionEntry Entry;
	Entry.Timestamp = Now;
	Entry.Source = ESomaPerceptionEntrySource::ASR;
	Entry.Text = RenderAsrBlock(Now, Trimmed);
	Entry.WordCount = CountWords(Entry.Text);

	TotalWords += Entry.WordCount;
	Entries.Add(MoveTemp(Entry));

	EvictWhileOverBudget();
	OnDynamicLogChanged.Broadcast();
	SaveDynamicLogToFile();
}

void USomaStorageSubsystem::AppendDynamicEntry(const FString& Text, const FString& SourceTag)
{
	const FString Trimmed = Text.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return;
	}

	const FDateTime Now = FDateTime::UtcNow();

	FString Block;
	Block.Reserve(64 + Trimmed.Len());
	Block += TEXT("[CUSTOM ");
	Block += Now.ToIso8601();
	Block += TEXT("]");
	if (!SourceTag.IsEmpty())
	{
		Block += TEXT(" (");
		Block += SourceTag;
		Block += TEXT(")");
	}
	Block += TEXT("\n");
	Block += Trimmed;
	Block += TEXT("\n");

	FSomaPerceptionEntry Entry;
	Entry.Timestamp = Now;
	Entry.Source = ESomaPerceptionEntrySource::Custom;
	Entry.Text = MoveTemp(Block);
	Entry.WordCount = CountWords(Entry.Text);

	TotalWords += Entry.WordCount;
	Entries.Add(MoveTemp(Entry));

	EvictWhileOverBudget();
	OnDynamicLogChanged.Broadcast();
	SaveDynamicLogToFile();
}

void USomaStorageSubsystem::SetAutoIngestEnabled(bool bEnabled)
{
	bAutoIngestEnabled = bEnabled;
}

FString USomaStorageSubsystem::BuildDynamicContext() const
{
	if (Entries.Num() == 0)
	{
		return FString();
	}

	FString Out;
	Out.Reserve(TotalWords * 8);

	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		if (i > 0)
		{
			Out += SomaStoragePrivate::EntrySeparator;
		}
		Out += Entries[i].Text;
	}

	return Out;
}

void USomaStorageSubsystem::ClearDynamicLog()
{
	if (Entries.Num() == 0 && TotalWords == 0)
	{
		return;
	}
	Entries.Reset();
	TotalWords = 0;
	OnDynamicLogChanged.Broadcast();
	SaveDynamicLogToFile();
}

void USomaStorageSubsystem::EvictWhileOverBudget()
{
	const int32 EntryCap = FMath::Max(1, MaxEntries);
	const int32 WordCap = FMath::Max(0, MaxWords);

	while (Entries.Num() > 0 && (Entries.Num() > EntryCap || (WordCap > 0 && TotalWords > WordCap)))
	{
		TotalWords = FMath::Max(0, TotalWords - Entries[0].WordCount);
		Entries.RemoveAt(0, 1, EAllowShrinking::No);
	}
}

int32 USomaStorageSubsystem::CountWords(const FString& Text)
{
	if (Text.IsEmpty())
	{
		return 0;
	}
	TArray<FString> Tokens;
	Text.ParseIntoArrayWS(Tokens);
	return Tokens.Num();
}

FString USomaStorageSubsystem::RenderVisionBlock(
	const FDateTime& When,
	const FSomaPerceptionSceneSummary& Summary,
	const TArray<FSomaPerceptionDetection>& Detections)
{
	FString Block;
	Block.Reserve(256 + Detections.Num() * 64);

	Block += TEXT("[VISION ");
	Block += When.ToIso8601();
	Block += TEXT("]\n");

	if (!Summary.Summary.IsEmpty())
	{
		Block += TEXT("Scene: ");
		Block += Summary.Summary;
		Block += TEXT("\n");
	}

	for (const FSomaPerceptionDetection& Det : Detections)
	{
		Block += TEXT("- ");

		const bool bHasDescription = !Det.Description.IsEmpty();
		if (bHasDescription)
		{
			Block += Det.Description;
		}
		else if (!Det.ClassName.IsEmpty())
		{
			Block += Det.ClassName;
		}
		else
		{
			Block += TEXT("(unknown)");
		}

		if (!Det.Action.IsEmpty())
		{
			Block += TEXT(" [");
			Block += Det.Action;
			Block += TEXT("]");
		}

		if (Det.Attributes.Num() > 0)
		{
			Block += TEXT(" (");
			Block += FString::Join(Det.Attributes, TEXT(", "));
			Block += TEXT(")");
		}

		Block += TEXT("\n");
	}

	return Block;
}

FString USomaStorageSubsystem::RenderAsrBlock(const FDateTime& When, const FString& Transcript)
{
	FString Block;
	Block.Reserve(64 + Transcript.Len());
	Block += TEXT("[ASR ");
	Block += When.ToIso8601();
	Block += TEXT("]\n");
	Block += Transcript;
	Block += TEXT("\n");
	return Block;
}

// ---------------------------------------------------------------------------
// Gaze
// ---------------------------------------------------------------------------

void USomaStorageSubsystem::UpdateGazeCandidates(const TArray<FSomaGazeCandidate>& Candidates)
{
	// The cache is always refreshed so prompt building / position resolution see
	// the freshest world positions, regardless of whether we log this tick.
	GazeCandidates = Candidates;

	// Build a coarse signature (name + position rounded to whole units) so we only
	// append a [GAZE] block to the rolling log when the set meaningfully changes.
	FString Signature;
	Signature.Reserve(GazeCandidates.Num() * 48);
	for (const FSomaGazeCandidate& Candidate : GazeCandidates)
	{
		Signature += Candidate.Name.ToLower();
		Signature += FString::Printf(TEXT("|%d,%d,%d;"),
			FMath::RoundToInt(Candidate.Position.X),
			FMath::RoundToInt(Candidate.Position.Y),
			FMath::RoundToInt(Candidate.Position.Z));
	}

	if (Signature == LastLoggedGazeSignature)
	{
		return;
	}
	LastLoggedGazeSignature = Signature;

	if (GazeCandidates.Num() == 0)
	{
		return;
	}

	const FDateTime Now = FDateTime::UtcNow();

	FSomaPerceptionEntry Entry;
	Entry.Timestamp = Now;
	Entry.Source = ESomaPerceptionEntrySource::Custom;
	Entry.Text = RenderGazeBlock(Now, GazeCandidates);
	Entry.WordCount = CountWords(Entry.Text);

	TotalWords += Entry.WordCount;
	Entries.Add(MoveTemp(Entry));

	EvictWhileOverBudget();
	OnDynamicLogChanged.Broadcast();
	SaveDynamicLogToFile();
}

bool USomaStorageSubsystem::ResolveGazePosition(const FString& Name, FVector& OutPosition) const
{
	if (Name.IsEmpty())
	{
		return false;
	}

	for (const FSomaGazeCandidate& Candidate : GazeCandidates)
	{
		if (Candidate.Name.Equals(Name, ESearchCase::IgnoreCase))
		{
			OutPosition = Candidate.Position;
			return true;
		}
	}

	return false;
}

FString USomaStorageSubsystem::RenderGazeBlock(const FDateTime& When, const TArray<FSomaGazeCandidate>& Candidates)
{
	FString Block;
	Block.Reserve(64 + Candidates.Num() * 64);
	Block += TEXT("[GAZE ");
	Block += When.ToIso8601();
	Block += TEXT("]\n");

	for (const FSomaGazeCandidate& Candidate : Candidates)
	{
		Block += TEXT("- ");
		Block += Candidate.Name;
		Block += FString::Printf(TEXT(" @ (%.0f, %.0f, %.0f)"),
			Candidate.Position.X, Candidate.Position.Y, Candidate.Position.Z);
		if (!Candidate.Description.IsEmpty())
		{
			Block += TEXT(" - ");
			Block += Candidate.Description;
		}
		Block += TEXT("\n");
	}

	return Block;
}

// ---------------------------------------------------------------------------
// Dynamic log file persistence
// ---------------------------------------------------------------------------

void USomaStorageSubsystem::SaveDynamicLogToFile()
{
	if (DynamicLogFilePath.IsEmpty())
	{
		return;
	}

	const FString Content = BuildDynamicContext();

	const FString Dir = FPaths::GetPath(DynamicLogFilePath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!Dir.IsEmpty() && !PlatformFile.DirectoryExists(*Dir))
	{
		PlatformFile.CreateDirectoryTree(*Dir);
	}

	if (!FFileHelper::SaveStringToFile(Content, *DynamicLogFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogSomaStorage, Warning, TEXT("Failed to save dynamic log to %s"), *DynamicLogFilePath);
	}
}

void USomaStorageSubsystem::LoadDynamicLogFromFile()
{
	if (DynamicLogFilePath.IsEmpty())
	{
		return;
	}

	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *DynamicLogFilePath))
	{
		return;
	}

	FileContent = FileContent.TrimStartAndEnd();
	if (FileContent.IsEmpty())
	{
		return;
	}

	Entries.Reset();
	TotalWords = 0;

	TArray<FString> Blocks;
	FileContent.ParseIntoArray(Blocks, TEXT("---"), true);

	for (FString& Block : Blocks)
	{
		Block = Block.TrimStartAndEnd();
		if (Block.IsEmpty())
		{
			continue;
		}

		FSomaPerceptionEntry Entry;
		Entry.Text = Block + TEXT("\n");
		Entry.WordCount = CountWords(Entry.Text);
		Entry.Timestamp = FDateTime::UtcNow();

		if (Block.StartsWith(TEXT("[VISION ")))
		{
			Entry.Source = ESomaPerceptionEntrySource::Vision;
		}
		else if (Block.StartsWith(TEXT("[ASR ")))
		{
			Entry.Source = ESomaPerceptionEntrySource::ASR;
		}
		else if (Block.StartsWith(TEXT("[CUSTOM ")))
		{
			Entry.Source = ESomaPerceptionEntrySource::Custom;
		}
		else
		{
			Entry.Source = ESomaPerceptionEntrySource::Custom;
		}

		// Try to parse the ISO-8601 timestamp from the header bracket.
		int32 SpaceIdx = INDEX_NONE;
		if (Block.FindChar(TEXT(' '), SpaceIdx) && SpaceIdx != INDEX_NONE)
		{
			int32 BracketEnd = INDEX_NONE;
			if (Block.FindChar(TEXT(']'), BracketEnd) && BracketEnd > SpaceIdx)
			{
				const FString TimestampStr = Block.Mid(SpaceIdx + 1, BracketEnd - SpaceIdx - 1);
				FDateTime Parsed;
				if (FDateTime::ParseIso8601(*TimestampStr, Parsed))
				{
					Entry.Timestamp = Parsed;
				}
			}
		}

		TotalWords += Entry.WordCount;
		Entries.Add(MoveTemp(Entry));
	}

	UE_LOG(LogSomaStorage, Log, TEXT("Loaded %d dynamic log entries (%d words) from %s"),
		Entries.Num(), TotalWords, *DynamicLogFilePath);
}

// ---------------------------------------------------------------------------
// Persistent text
// ---------------------------------------------------------------------------

void USomaStorageSubsystem::EnsurePersistentFileExists()
{
	const FString Dir = FPaths::GetPath(PersistentTextPath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!Dir.IsEmpty() && !PlatformFile.DirectoryExists(*Dir))
	{
		PlatformFile.CreateDirectoryTree(*Dir);
	}

	if (!PlatformFile.FileExists(*PersistentTextPath))
	{
		FFileHelper::SaveStringToFile(FString(), *PersistentTextPath);
	}
}

void USomaStorageSubsystem::ReloadPersistentText()
{
	FString Loaded;
	if (FFileHelper::LoadFileToString(Loaded, *PersistentTextPath))
	{
		if (Loaded != PersistentText)
		{
			PersistentText = MoveTemp(Loaded);
			OnPersistentTextChanged.Broadcast(PersistentText);
		}
	}
	else
	{
		UE_LOG(LogSomaStorage, Warning, TEXT("Failed to load persistent text from %s"), *PersistentTextPath);
	}
}

void USomaStorageSubsystem::RegisterDirectoryWatcher()
{
#if WITH_EDITOR
	WatchedDirectory = FPaths::GetPath(PersistentTextPath);
	if (WatchedDirectory.IsEmpty())
	{
		return;
	}

	FDirectoryWatcherModule& Module = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
	IDirectoryWatcher* Watcher = Module.Get();
	if (!Watcher)
	{
		UE_LOG(LogSomaStorage, Warning, TEXT("DirectoryWatcher unavailable; persistent file hot-reload disabled."));
		return;
	}

	const bool bOk = Watcher->RegisterDirectoryChangedCallback_Handle(
		WatchedDirectory,
		IDirectoryWatcher::FDirectoryChanged::CreateUObject(this, &USomaStorageSubsystem::OnPersistentDirectoryChanged),
		DirectoryWatcherHandle,
		IDirectoryWatcher::WatchOptions::IncludeDirectoryChanges);

	if (!bOk)
	{
		UE_LOG(LogSomaStorage, Warning, TEXT("Failed to register directory watcher for %s"), *WatchedDirectory);
	}
#else
	PersistentTextTimestamp = IFileManager::Get().GetTimeStamp(*PersistentTextPath);
	PromptConfigTimestamp = IFileManager::Get().GetTimeStamp(*PromptConfigPath);
	FileWatcherTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &USomaStorageSubsystem::TickFileWatcher), 1.0f);
#endif
}

void USomaStorageSubsystem::UnregisterDirectoryWatcher()
{
#if WITH_EDITOR
	if (WatchedDirectory.IsEmpty() || !DirectoryWatcherHandle.IsValid())
	{
		return;
	}

	if (FModuleManager::Get().IsModuleLoaded(TEXT("DirectoryWatcher")))
	{
		FDirectoryWatcherModule& Module = FModuleManager::GetModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
		if (IDirectoryWatcher* Watcher = Module.Get())
		{
			Watcher->UnregisterDirectoryChangedCallback_Handle(WatchedDirectory, DirectoryWatcherHandle);
		}
	}

	DirectoryWatcherHandle.Reset();
	WatchedDirectory.Reset();
#else
	if (FileWatcherTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(FileWatcherTickerHandle);
		FileWatcherTickerHandle.Reset();
	}
#endif
}

#if WITH_EDITOR
void USomaStorageSubsystem::OnPersistentDirectoryChanged(const TArray<FFileChangeData>& FileChanges)
{
	const FString PersistentFull = FPaths::ConvertRelativePathToFull(PersistentTextPath);
	const FString PromptConfigFull = FPaths::ConvertRelativePathToFull(PromptConfigPath);

	for (const FFileChangeData& Change : FileChanges)
	{
		const FString ChangedFull = FPaths::ConvertRelativePathToFull(Change.Filename);
		if (ChangedFull.Equals(PersistentFull, ESearchCase::IgnoreCase))
		{
			ReloadPersistentText();
		}
		else if (ChangedFull.Equals(PromptConfigFull, ESearchCase::IgnoreCase))
		{
			ReloadPromptConfig();
		}
	}
}
#else
bool USomaStorageSubsystem::TickFileWatcher(float DeltaTime)
{
	const FDateTime NewPersistentTimestamp = IFileManager::Get().GetTimeStamp(*PersistentTextPath);
	if (NewPersistentTimestamp != PersistentTextTimestamp)
	{
		PersistentTextTimestamp = NewPersistentTimestamp;
		ReloadPersistentText();
	}

	const FDateTime NewPromptConfigTimestamp = IFileManager::Get().GetTimeStamp(*PromptConfigPath);
	if (NewPromptConfigTimestamp != PromptConfigTimestamp)
	{
		PromptConfigTimestamp = NewPromptConfigTimestamp;
		ReloadPromptConfig();
	}

	return true;
}
#endif

// ---------------------------------------------------------------------------
// Prompt config persistence
// ---------------------------------------------------------------------------

void USomaStorageSubsystem::EnsurePromptConfigFileExists()
{
	const FString Dir = FPaths::GetPath(PromptConfigPath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!Dir.IsEmpty() && !PlatformFile.DirectoryExists(*Dir))
	{
		PlatformFile.CreateDirectoryTree(*Dir);
	}

	if (!PlatformFile.FileExists(*PromptConfigPath))
	{
		const FString DefaultJson = TEXT(
			"{\n"
			"  \"character_name\": \"Assistant\",\n"
			"  \"character_description\": \"A helpful assistant in a video game.\",\n"
			"  \"character_goal\": \"Help the player.\",\n"
			"  \"available_actions\": [\n"
			"    { \"tag\": \"idle\", \"event\": \"none\", \"description\": \"Stay in place\" }\n"
			"  ]\n"
			"}"
		);
		FFileHelper::SaveStringToFile(DefaultJson, *PromptConfigPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

void USomaStorageSubsystem::ReloadPromptConfig()
{
	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *PromptConfigPath))
	{
		UE_LOG(LogSomaStorage, Warning, TEXT("Failed to load prompt config from %s"), *PromptConfigPath);
		return;
	}

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogSomaStorage, Warning, TEXT("Failed to parse prompt_config.json"));
		return;
	}

	PromptConfig.CharacterName = JsonObject->GetStringField(TEXT("character_name"));
	PromptConfig.CharacterDescription = JsonObject->GetStringField(TEXT("character_description"));
	PromptConfig.CharacterGoal = JsonObject->GetStringField(TEXT("character_goal"));
	PromptConfig.AvailableActions.Reset();

	const TArray<TSharedPtr<FJsonValue>>* ActionsArray = nullptr;
	if (JsonObject->TryGetArrayField(TEXT("available_actions"), ActionsArray))
	{
		for (const TSharedPtr<FJsonValue>& Val : *ActionsArray)
		{
			const TSharedPtr<FJsonObject> ActionObj = Val->AsObject();
			if (!ActionObj.IsValid())
			{
				continue;
			}

			FSomaActionMapping Mapping;
			Mapping.Tag = ActionObj->GetStringField(TEXT("tag"));
			Mapping.Event = ActionObj->GetStringField(TEXT("event"));
			Mapping.Description = ActionObj->GetStringField(TEXT("description"));
			PromptConfig.AvailableActions.Add(MoveTemp(Mapping));
		}
	}

	UE_LOG(LogSomaStorage, Log, TEXT("Loaded prompt config: %s (%d actions)"),
		*PromptConfig.CharacterName, PromptConfig.AvailableActions.Num());
}

// ---------------------------------------------------------------------------
// Structured prompt builder
// ---------------------------------------------------------------------------

FString USomaStorageSubsystem::BuildStructuredSystemPrompt() const
{
	FString Out;
	Out.Reserve(2048);

	Out += TEXT("You are ");
	Out += PromptConfig.CharacterName;
	Out += TEXT(". Respond ONLY with a JSON object in the exact format shown below.\n\n");

	Out += TEXT("## Identity\n");
	Out += PromptConfig.CharacterDescription;
	Out += TEXT("\n\n");

	Out += TEXT("## Goal\n");
	Out += PromptConfig.CharacterGoal;
	Out += TEXT("\n\n");

	Out += TEXT("## Available Actions\nSelect ONE action from this list by tag name.\n");
	for (const FSomaActionMapping& Action : PromptConfig.AvailableActions)
	{
		Out += TEXT("- ");
		Out += Action.Tag;
		Out += TEXT(" (");
		Out += Action.Event;
		Out += TEXT("): ");
		Out += Action.Description;
		Out += TEXT("\n");
	}
	Out += TEXT("\n");

	if (GazeCandidates.Num() > 0)
	{
		Out += TEXT("## Visible Objects\nObjects you can choose to look at. Use the name EXACTLY as written:\n");
		for (const FSomaGazeCandidate& Candidate : GazeCandidates)
		{
			Out += TEXT("- ");
			Out += Candidate.Name;
			if (!Candidate.Description.IsEmpty())
			{
				Out += TEXT(": ");
				Out += Candidate.Description;
			}
			Out += TEXT("\n");
		}
		Out += TEXT("\n");

		Out += TEXT("## Gaze\n");
		Out += TEXT("Always identify the single most suitable object to look at given the context; choose its name EXACTLY as listed under Visible Objects, or empty string if none fits.\n\n");
	}

	const FString DynamicContext = BuildDynamicContext();
	if (!DynamicContext.IsEmpty())
	{
		Out += TEXT("## Context\n");
		Out += DynamicContext;
		Out += TEXT("\n\n");
	}

	Out += TEXT("## Output Format\n");
	Out += TEXT("Respond with ONLY a JSON object containing these fields:\n");
	Out += TEXT("{\n");
	Out += TEXT("  \"reasoning\": \"<your chain of thought>\",\n");
	Out += TEXT("  \"dialogue\": \"<spoken text>\",\n");
	Out += TEXT("  \"action\": { \"tag\": \"<tag from Available Actions>\", \"event\": \"<corresponding event>\" },\n");
	Out += TEXT("  \"gaze\": { \"object\": \"<name from Visible Objects, or empty>\" }\n");
	Out += TEXT("}\n");

	return Out;
}

FSomaStructuredResponse USomaStorageSubsystem::ParseStructuredResponse(const FString& JsonString)
{
	FSomaStructuredResponse Result;

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogSomaStorage, Warning, TEXT("ParseStructuredResponse: failed to parse JSON"));
		return Result;
	}

	Result.Reasoning = JsonObject->GetStringField(TEXT("reasoning"));
	Result.Dialogue = JsonObject->GetStringField(TEXT("dialogue"));

	const TSharedPtr<FJsonObject>* ActionObj = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("action"), ActionObj) && ActionObj->IsValid())
	{
		Result.ActionTag = (*ActionObj)->GetStringField(TEXT("tag"));
		Result.ActionEvent = (*ActionObj)->GetStringField(TEXT("event"));
	}

	const TSharedPtr<FJsonObject>* GazeObj = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("gaze"), GazeObj) && GazeObj->IsValid())
	{
		Result.GazeObject = (*GazeObj)->GetStringField(TEXT("object")).TrimStartAndEnd();
	}
	Result.bHasGazeTarget = !Result.GazeObject.IsEmpty();

	return Result;
}

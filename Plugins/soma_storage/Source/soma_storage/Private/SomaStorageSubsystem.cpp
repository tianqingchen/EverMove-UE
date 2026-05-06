#include "SomaStorageSubsystem.h"

#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

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

	EnsurePersistentFileExists();
	ReloadPersistentText();
	RegisterDirectoryWatcher();

	UE_LOG(LogSomaStorage, Log,
		TEXT("SomaStorageSubsystem initialized. PersistentTextPath=%s, MaxWords=%d, MaxEntries=%d"),
		*PersistentTextPath, MaxWords, MaxEntries);
}

void USomaStorageSubsystem::Deinitialize()
{
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
}

void USomaStorageSubsystem::AddAsrTranscript(const FString& Transcript)
{
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
}

void USomaStorageSubsystem::UnregisterDirectoryWatcher()
{
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
}

void USomaStorageSubsystem::OnPersistentDirectoryChanged(const TArray<FFileChangeData>& FileChanges)
{
	const FString TargetFull = FPaths::ConvertRelativePathToFull(PersistentTextPath);

	for (const FFileChangeData& Change : FileChanges)
	{
		const FString ChangedFull = FPaths::ConvertRelativePathToFull(Change.Filename);
		if (ChangedFull.Equals(TargetFull, ESearchCase::IgnoreCase))
		{
			ReloadPersistentText();
			return;
		}
	}
}

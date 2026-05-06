#pragma once

#include "CoreMinimal.h"
#include "SomaDialogueTypes.generated.h"

UENUM(BlueprintType)
enum class ESomaDialogueRole : uint8
{
	/** System role (reserved for JSON; history uses `SystemPrompt` instead). */
	DialogueSystem UMETA(DisplayName = "System"),
	User UMETA(DisplayName = "User"),
	Assistant UMETA(DisplayName = "Assistant")
};

USTRUCT(BlueprintType)
struct FSomaDialogueMessage
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaDialogue")
	ESomaDialogueRole Role = ESomaDialogueRole::User;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaDialogue")
	FString Content;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSomaDialogueOnDialogueGenerated, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSomaDialogueOnValidationComplete, bool, bSuccess, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSomaDialogueOnStatusMessage, const FString&, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSomaDialogueOnError, const FString&, Error);

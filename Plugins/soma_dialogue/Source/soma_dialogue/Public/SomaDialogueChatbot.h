#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Http.h"
#include "SomaDialogueTypes.h"
#include "SomaDialogueChatbot.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSomaDialogue, Log, All);

/**
 * ASomaDialogueChatbot
 *
 * LLM-only chat actor: text in (`RequestDialogue`) -> Chat Completions -> text out
 * via `OnDialogueGenerated`. Audio capture, Whisper, and the mic device dropdown
 * have moved to `ASomaPerceptionActor`. Wire `OnUserTranscript` on the perception
 * actor to `RequestDialogue` here for the typical mic / text -> reply flow.
 */
UCLASS()
class SOMA_DIALOGUE_API ASomaDialogueChatbot : public AActor
{
	GENERATED_BODY()

public:
	ASomaDialogueChatbot();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	// ---- Config ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaDialogue|Config")
	FString APIKey;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaDialogue|Config")
	bool bAutoValidateApiKeyOnBeginPlay = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaDialogue|Config")
	FString ChatModel = TEXT("gpt-4o-mini");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaDialogue|Config", meta = (MultiLine = true))
	FString SystemPrompt = TEXT("You are a helpful assistant in a video game. Keep answers short and conversational.");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaDialogue|Config")
	int32 MaxHistoryTurns = 10;

	// ---- Blueprint API ----

	/**
	 * Send a user line to Chat Completions and broadcast the reply via OnDialogueGenerated.
	 * Equivalent to the previous SubmitText, minus the OnUserTranscript broadcast (that
	 * lives on ASomaPerceptionActor now).
	 */
	UFUNCTION(BlueprintCallable, Category = "SomaDialogue")
	void RequestDialogue(const FString& UserText);

	UFUNCTION(BlueprintCallable, Category = "SomaDialogue")
	void ValidateAPIKey(const FString& Key);

	UFUNCTION(BlueprintCallable, Category = "SomaDialogue")
	bool IsApiKeyValidated() const { return bApiKeyValidated; }

	UFUNCTION(BlueprintCallable, Category = "SomaDialogue")
	void ResetConversation();

	/** Cancels the in-flight chat request (chat-only — perception owns the Whisper request). */
	UFUNCTION(BlueprintCallable, Category = "SomaDialogue")
	void CancelInFlight();

	// ---- Delegates ----

	UPROPERTY(BlueprintAssignable, Category = "SomaDialogue|Events")
	FSomaDialogueOnDialogueGenerated OnDialogueGenerated;

	UPROPERTY(BlueprintAssignable, Category = "SomaDialogue|Events")
	FSomaDialogueOnValidationComplete OnValidationComplete;

	UPROPERTY(BlueprintAssignable, Category = "SomaDialogue|Events")
	FSomaDialogueOnStatusMessage OnStatusMessage;

	UPROPERTY(BlueprintAssignable, Category = "SomaDialogue|Events")
	FSomaDialogueOnError OnError;

private:
	void OnModelsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	void OnChatCompletionsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	void TrimConversationHistory();

	TArray<FSomaDialogueMessage> ConversationHistory;

	bool bApiKeyValidated = false;
	FString PendingValidationKey;

	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveChatRequest;
};

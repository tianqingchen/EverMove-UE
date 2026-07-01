#include "SomaDialogueChatbot.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"
#include "SomaStorageSubsystem.h"

DEFINE_LOG_CATEGORY(LogSomaDialogue);

namespace SomaDialogueChatbotPrivate
{
	static FString RoleToOpenAIRole(const ESomaDialogueRole Role)
	{
		switch (Role)
		{
		case ESomaDialogueRole::DialogueSystem:
			return TEXT("system");
		case ESomaDialogueRole::User:
			return TEXT("user");
		case ESomaDialogueRole::Assistant:
			return TEXT("assistant");
		default:
			return TEXT("user");
		}
	}
}

ASomaDialogueChatbot::ASomaDialogueChatbot()
{
	PrimaryActorTick.bCanEverTick = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
}

void ASomaDialogueChatbot::BeginPlay()
{
	Super::BeginPlay();

	if (bAutoValidateApiKeyOnBeginPlay && !APIKey.IsEmpty() && !bApiKeyValidated)
	{
		ValidateAPIKey(APIKey);
	}
}

void ASomaDialogueChatbot::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	CancelInFlight();
	Super::EndPlay(EndPlayReason);
}

void ASomaDialogueChatbot::RequestDialogue(const FString& UserText)
{
	const FString Trimmed = UserText.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		OnError.Broadcast(TEXT("RequestDialogue was called with empty text."));
		return;
	}

	if (APIKey.IsEmpty())
	{
		OnError.Broadcast(TEXT("OpenAI API key is not set."));
		return;
	}

	if (ActiveChatRequest.IsValid())
	{
		ActiveChatRequest->CancelRequest();
		ActiveChatRequest.Reset();
	}

	OnStatusMessage.Broadcast(TEXT("Thinking..."));

	FHttpModule& Http = FHttpModule::Get();
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http.CreateRequest();
	ActiveChatRequest = Request;

	Request->SetURL(TEXT("https://api.openai.com/v1/chat/completions"));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Authorization"), FString(TEXT("Bearer ")) + APIKey);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetStringField(TEXT("model"), ChatModel);

	TArray<TSharedPtr<FJsonValue>> Messages;

	if (bUseStructuredPrompt)
	{
		USomaStorageSubsystem* Storage = nullptr;
		if (const UGameInstance* GI = GetGameInstance())
		{
			Storage = GI->GetSubsystem<USomaStorageSubsystem>();
		}

		FString StructuredSystemPrompt;
		if (Storage)
		{
			StructuredSystemPrompt = Storage->BuildStructuredSystemPrompt();
		}
		else
		{
			UE_LOG(LogSomaDialogue, Warning, TEXT("bUseStructuredPrompt is true but SomaStorageSubsystem is unavailable. Falling back to flat prompt."));
			StructuredSystemPrompt = SystemPrompt;
		}

		TSharedPtr<FJsonObject> SystemMsg = MakeShareable(new FJsonObject());
		SystemMsg->SetStringField(TEXT("role"), TEXT("system"));
		SystemMsg->SetStringField(TEXT("content"), StructuredSystemPrompt);
		Messages.Add(MakeShareable(new FJsonValueObject(SystemMsg)));

		TSharedPtr<FJsonObject> UserMsg = MakeShareable(new FJsonObject());
		UserMsg->SetStringField(TEXT("role"), TEXT("user"));
		UserMsg->SetStringField(TEXT("content"), Trimmed);
		Messages.Add(MakeShareable(new FJsonValueObject(UserMsg)));

		TSharedPtr<FJsonObject> ResponseFormat = MakeShareable(new FJsonObject());
		ResponseFormat->SetStringField(TEXT("type"), TEXT("json_object"));
		Root->SetObjectField(TEXT("response_format"), ResponseFormat);
	}
	else
	{
		FSomaDialogueMessage UserMsg;
		UserMsg.Role = ESomaDialogueRole::User;
		UserMsg.Content = Trimmed;
		ConversationHistory.Add(UserMsg);
		TrimConversationHistory();

		TSharedPtr<FJsonObject> SystemMsg = MakeShareable(new FJsonObject());
		SystemMsg->SetStringField(TEXT("role"), TEXT("system"));
		SystemMsg->SetStringField(TEXT("content"), SystemPrompt);
		Messages.Add(MakeShareable(new FJsonValueObject(SystemMsg)));

		for (const FSomaDialogueMessage& Msg : ConversationHistory)
		{
			TSharedPtr<FJsonObject> JsonMsg = MakeShareable(new FJsonObject());
			JsonMsg->SetStringField(TEXT("role"), SomaDialogueChatbotPrivate::RoleToOpenAIRole(Msg.Role));
			JsonMsg->SetStringField(TEXT("content"), Msg.Content);
			Messages.Add(MakeShareable(new FJsonValueObject(JsonMsg)));
		}
	}

	Root->SetArrayField(TEXT("messages"), Messages);

	FString OutputString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	Request->SetContentAsString(OutputString);
	Request->OnProcessRequestComplete().BindUObject(this, &ASomaDialogueChatbot::OnChatCompletionsResponse);
	Request->ProcessRequest();
}

void ASomaDialogueChatbot::ValidateAPIKey(const FString& Key)
{
	const FString Trimmed = Key.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		OnValidationComplete.Broadcast(false, TEXT("Please enter an API key."));
		return;
	}

	PendingValidationKey = Trimmed;
	OnStatusMessage.Broadcast(TEXT("Validating API key..."));

	FHttpModule& Http = FHttpModule::Get();
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http.CreateRequest();
	Request->SetURL(TEXT("https://api.openai.com/v1/models"));
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Trimmed);
	Request->OnProcessRequestComplete().BindUObject(this, &ASomaDialogueChatbot::OnModelsResponse);
	Request->ProcessRequest();
}

void ASomaDialogueChatbot::OnModelsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	if (!bWasSuccessful || !Response.IsValid())
	{
		PendingValidationKey.Empty();
		OnValidationComplete.Broadcast(false, TEXT("Network error while contacting OpenAI."));
		OnStatusMessage.Broadcast(TEXT("Validation failed (network)."));
		return;
	}

	if (Response->GetResponseCode() != 200)
	{
		PendingValidationKey.Empty();
		const FString Msg = FString::Printf(
			TEXT("HTTP %d: %s"), Response->GetResponseCode(), *Response->GetContentAsString());
		OnValidationComplete.Broadcast(false, Msg);
		OnStatusMessage.Broadcast(TEXT("Invalid API key or access denied."));
		return;
	}

	APIKey = PendingValidationKey;
	PendingValidationKey.Empty();
	bApiKeyValidated = true;
	OnValidationComplete.Broadcast(true, TEXT("API key accepted."));
	OnStatusMessage.Broadcast(TEXT("API key validated."));
}

void ASomaDialogueChatbot::OnChatCompletionsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	if (!ActiveChatRequest.IsValid() || ActiveChatRequest.Get() != Request.Get())
	{
		return;
	}
	ActiveChatRequest.Reset();

	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogSomaDialogue, Error, TEXT("Chat completions request failed (network)."));
		OnError.Broadcast(TEXT("Chat request failed (network)."));
		return;
	}

	if (Response->GetResponseCode() != 200)
	{
		UE_LOG(LogSomaDialogue, Error, TEXT("Chat completions error: %s"), *Response->GetContentAsString());
		OnError.Broadcast(FString::Printf(TEXT("Chat HTTP %d — check model name and account."), Response->GetResponseCode()));
		return;
	}

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		OnError.Broadcast(TEXT("Failed to parse chat response JSON."));
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
	if (!JsonObject->TryGetArrayField(TEXT("choices"), Choices) || Choices->Num() == 0)
	{
		OnError.Broadcast(TEXT("Chat response missing choices."));
		return;
	}

	const TSharedPtr<FJsonObject> FirstChoice = (*Choices)[0]->AsObject();
	if (!FirstChoice.IsValid())
	{
		OnError.Broadcast(TEXT("Invalid choice object in chat response."));
		return;
	}

	const TSharedPtr<FJsonObject> Message = FirstChoice->GetObjectField(TEXT("message"));
	if (!Message.IsValid())
	{
		OnError.Broadcast(TEXT("Chat response missing message."));
		return;
	}

	const FString Content = Message->GetStringField(TEXT("content"));

	if (bUseStructuredPrompt)
	{
		FSomaStructuredResponse Parsed = USomaStorageSubsystem::ParseStructuredResponse(Content);

		UE_LOG(LogSomaDialogue, Log, TEXT("Structured reasoning: %s"), *Parsed.Reasoning);

		OnDialogueGenerated.Broadcast(Parsed.Dialogue);
		OnStructuredDialogueGenerated.Broadcast(Parsed.Dialogue, Parsed.ActionTag, Parsed.ActionEvent);

		if (const UGameInstance* GI = GetGameInstance())
		{
			if (USomaStorageSubsystem* Storage = GI->GetSubsystem<USomaStorageSubsystem>())
			{
				if (Parsed.bHasGazeTarget)
				{
					// Resolve the authoritative world position engine-side; the LLM only returns a name.
					Storage->ResolveGazePosition(Parsed.GazeObject, Parsed.GazePosition);
					OnGazeTargetSelected.Broadcast(Parsed.GazeObject, Parsed.GazePosition);
				}

				Storage->AppendDynamicEntry(Content, TEXT("AssistantDialogue"));
			}
		}
	}
	else
	{
		FSomaDialogueMessage AssistantMsg;
		AssistantMsg.Role = ESomaDialogueRole::Assistant;
		AssistantMsg.Content = Content;
		ConversationHistory.Add(AssistantMsg);
		TrimConversationHistory();

		OnDialogueGenerated.Broadcast(Content);
	}

	OnStatusMessage.Broadcast(TEXT("Dialogue ready."));
}

void ASomaDialogueChatbot::TrimConversationHistory()
{
	if (MaxHistoryTurns <= 0)
	{
		return;
	}

	while (ConversationHistory.Num() > 2 * MaxHistoryTurns)
	{
		ConversationHistory.RemoveAt(0, 2, EAllowShrinking::No);
	}
}

void ASomaDialogueChatbot::ResetConversation()
{
	ConversationHistory.Reset();
	OnStatusMessage.Broadcast(TEXT("Conversation reset."));
}

void ASomaDialogueChatbot::CancelInFlight()
{
	if (ActiveChatRequest.IsValid())
	{
		ActiveChatRequest->CancelRequest();
		ActiveChatRequest.Reset();
	}
}

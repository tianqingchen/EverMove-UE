#include "OpenAiVisionClient.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"

// ─── public ───────────────────────────────────────────────────────

void FOpenAiVisionClient::SendFrame(
	const TArray<uint8>& RGBPixels,
	int32 Width,
	int32 Height,
	const FString& ApiKey,
	const FString& Model,
	float TimeoutSeconds,
	FOnOpenAiVisionResponse OnComplete)
{
	const TArray<uint8> JpegBytes = EncodeJpeg(RGBPixels, Width, Height);
	if (JpegBytes.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("soma_perception: OpenAI – JPEG encode failed"));
		OnComplete.ExecuteIfBound({}, FSomaPerceptionSceneSummary(), 0.0f, false);
		return;
	}

	const FString Base64 = FBase64::Encode(JpegBytes);
	const FString Body = BuildRequestBody(Base64, Model);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(TEXT("https://api.openai.com/v1/responses"));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	Request->SetContentAsString(Body);
	Request->SetTimeout(TimeoutSeconds);

	const double StartTime = FPlatformTime::Seconds();
	const int32 FrameW = Width;
	const int32 FrameH = Height;

	TWeakPtr<FOpenAiVisionClient> WeakSelf = AsShared();

	Request->OnProcessRequestComplete().BindLambda(
		[WeakSelf, OnComplete, StartTime, FrameW, FrameH]
		(FHttpRequestPtr /*Req*/, FHttpResponsePtr Response, bool bConnectedSuccessfully)
		{
			const float Latency = static_cast<float>(FPlatformTime::Seconds() - StartTime);

			if (!bConnectedSuccessfully || !Response.IsValid())
			{
				UE_LOG(LogTemp, Warning, TEXT("soma_perception: OpenAI request failed (no response)"));
				OnComplete.ExecuteIfBound({}, FSomaPerceptionSceneSummary(), Latency, false);
				return;
			}

			const int32 Code = Response->GetResponseCode();
			if (Code != 200)
			{
				UE_LOG(LogTemp, Warning, TEXT("soma_perception: OpenAI HTTP %d – %s"),
					Code, *Response->GetContentAsString().Left(512));
				OnComplete.ExecuteIfBound({}, FSomaPerceptionSceneSummary(), Latency, false);
				return;
			}

			FSomaPerceptionSceneSummary SceneSummary;
			TArray<FSomaPerceptionDetection> Detections = ParseDetections(
				Response->GetContentAsString(), FrameW, FrameH, SceneSummary);

			OnComplete.ExecuteIfBound(MoveTemp(Detections), SceneSummary, Latency, true);
		});

	Request->ProcessRequest();
}

// ─── JPEG encoding via IImageWrapper ──────────────────────────────

TArray<uint8> FOpenAiVisionClient::EncodeJpeg(
	const TArray<uint8>& RGBPixels, int32 Width, int32 Height)
{
	IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Wrapper = ImageModule.CreateImageWrapper(EImageFormat::JPEG);
	if (!Wrapper.IsValid())
	{
		return {};
	}

	const int32 NumPixels = Width * Height;

	// IImageWrapper expects RGBA (4 bpp); expand from RGB (3 bpp).
	TArray<uint8> RGBAPixels;
	RGBAPixels.SetNumUninitialized(NumPixels * 4);
	for (int32 i = 0; i < NumPixels; ++i)
	{
		RGBAPixels[i * 4 + 0] = RGBPixels[i * 3 + 0];
		RGBAPixels[i * 4 + 1] = RGBPixels[i * 3 + 1];
		RGBAPixels[i * 4 + 2] = RGBPixels[i * 3 + 2];
		RGBAPixels[i * 4 + 3] = 255;
	}

	if (!Wrapper->SetRaw(RGBAPixels.GetData(), RGBAPixels.Num(),
		Width, Height, ERGBFormat::RGBA, 8))
	{
		return {};
	}

	TArray64<uint8> CompressedLarge = Wrapper->GetCompressed(85);
	if (CompressedLarge.Num() == 0)
	{
		return {};
	}

	TArray<uint8> Compressed;
	Compressed.SetNumUninitialized(static_cast<int32>(CompressedLarge.Num()));
	FMemory::Memcpy(Compressed.GetData(), CompressedLarge.GetData(), CompressedLarge.Num());
	return Compressed;
}

// ─── request body ─────────────────────────────────────────────────

FString FOpenAiVisionClient::BuildRequestBody(
	const FString& Base64Image, const FString& Model)
{
	// Responses API format: model + input array with role/content items.
	// The prompt enforces a strict JSON schema for detections.
	const FString Prompt = TEXT(
		"You are an object-detection model. Analyze the image and return ONLY a JSON object "
		"(no markdown, no commentary) with this exact shape: "
		"{\"scene_summary\":\"<1-2 sentence description of the whole scene>\","
		"\"detections\":[{\"class_name\":\"<label>\",\"confidence\":<0-1>,"
		"\"description\":\"<natural-language sentence about this object>\","
		"\"attributes\":[\"<tag1>\",\"<tag2>\"],"
		"\"action\":\"<what the object is doing>\","
		"\"bbox_min\":{\"x\":<int>,\"y\":<int>},"
		"\"bbox_max\":{\"x\":<int>,\"y\":<int>}}]}. "
		"Coordinates are in the image's original pixel space. "
		"If nothing is detected, return {\"scene_summary\":\"\",\"detections\":[]}.");

	const FString ImageUrl = FString::Printf(
		TEXT("data:image/jpeg;base64,%s"), *Base64Image);

	// Build manually so we control the exact shape expected by the Responses API.
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("model"), Model);

	// input array
	TArray<TSharedPtr<FJsonValue>> InputArr;

	// single user message item
	TSharedRef<FJsonObject> UserItem = MakeShared<FJsonObject>();
	UserItem->SetStringField(TEXT("role"), TEXT("user"));

	TArray<TSharedPtr<FJsonValue>> ContentArr;

	// text part
	TSharedRef<FJsonObject> TextPart = MakeShared<FJsonObject>();
	TextPart->SetStringField(TEXT("type"), TEXT("input_text"));
	TextPart->SetStringField(TEXT("text"), Prompt);
	ContentArr.Add(MakeShared<FJsonValueObject>(TextPart));

	// image part
	TSharedRef<FJsonObject> ImgPart = MakeShared<FJsonObject>();
	ImgPart->SetStringField(TEXT("type"), TEXT("input_image"));
	ImgPart->SetStringField(TEXT("image_url"), ImageUrl);
	ContentArr.Add(MakeShared<FJsonValueObject>(ImgPart));

	UserItem->SetArrayField(TEXT("content"), ContentArr);
	InputArr.Add(MakeShared<FJsonValueObject>(UserItem));

	Root->SetArrayField(TEXT("input"), InputArr);

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, Writer);
	return Out;
}

// ─── response parsing ─────────────────────────────────────────────

TArray<FSomaPerceptionDetection> FOpenAiVisionClient::ParseDetections(
	const FString& JsonContent, int32 FrameWidth, int32 FrameHeight, FSomaPerceptionSceneSummary& OutSceneSummary)
{
	OutSceneSummary = FSomaPerceptionSceneSummary();

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonContent);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("soma_perception: OpenAI – failed to parse root JSON"));
		return {};
	}

	// Responses API wraps output in an "output" array with message items.
	const TArray<TSharedPtr<FJsonValue>>* OutputArr = nullptr;
	if (!Root->TryGetArrayField(TEXT("output"), OutputArr) || !OutputArr)
	{
		UE_LOG(LogTemp, Warning, TEXT("soma_perception: OpenAI – missing 'output' array"));
		return {};
	}

	// Walk output items to find the assistant message with output_text content.
	for (const TSharedPtr<FJsonValue>& OutputVal : *OutputArr)
	{
		const TSharedPtr<FJsonObject>* OutputObj = nullptr;
		if (!OutputVal->TryGetObject(OutputObj) || !OutputObj || !(*OutputObj)->HasField(TEXT("content")))
		{
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* ContentArr = nullptr;
		if (!(*OutputObj)->TryGetArrayField(TEXT("content"), ContentArr) || !ContentArr)
		{
			continue;
		}

		for (const TSharedPtr<FJsonValue>& ContentVal : *ContentArr)
		{
			const TSharedPtr<FJsonObject>* ContentObj = nullptr;
			if (!ContentVal->TryGetObject(ContentObj) || !ContentObj)
			{
				continue;
			}

			FString Type;
			(*ContentObj)->TryGetStringField(TEXT("type"), Type);
			if (Type != TEXT("output_text"))
			{
				continue;
			}

			FString Text;
			if (!(*ContentObj)->TryGetStringField(TEXT("text"), Text))
			{
				continue;
			}

			// The model text should be a JSON array. Try to extract it even if
			// wrapped in markdown code fences or additional text.
			const int32 ObjectStart = Text.Find(TEXT("{"));
			const int32 ObjectEnd = Text.Find(TEXT("}"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			if (ObjectStart != INDEX_NONE && ObjectEnd != INDEX_NONE && ObjectEnd > ObjectStart)
			{
				const FString ObjectStr = Text.Mid(ObjectStart, ObjectEnd - ObjectStart + 1);
				TSharedPtr<FJsonObject> ParsedObj;
				TSharedRef<TJsonReader<>> ObjReader = TJsonReaderFactory<>::Create(ObjectStr);
				if (FJsonSerializer::Deserialize(ObjReader, ParsedObj) && ParsedObj.IsValid())
				{
					ParsedObj->TryGetStringField(TEXT("scene_summary"), OutSceneSummary.Summary);
					const TArray<TSharedPtr<FJsonValue>>* WrappedArray = nullptr;
					if (ParsedObj->TryGetArrayField(TEXT("detections"), WrappedArray) && WrappedArray)
					{
						TArray<FSomaPerceptionDetection> ParsedDetections = ParseDetectionArray(*WrappedArray, FrameWidth, FrameHeight);
						OutSceneSummary.ObjectCount = ParsedDetections.Num();
						return ParsedDetections;
					}
				}
			}

			const int32 BracketStart = Text.Find(TEXT("["));
			const int32 BracketEnd = Text.Find(TEXT("]"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			if (BracketStart != INDEX_NONE && BracketEnd != INDEX_NONE && BracketEnd > BracketStart)
			{
				const FString ArrayStr = Text.Mid(BracketStart, BracketEnd - BracketStart + 1);
				TArray<TSharedPtr<FJsonValue>> DetArray;
				TSharedRef<TJsonReader<>> ArrReader = TJsonReaderFactory<>::Create(ArrayStr);
				if (FJsonSerializer::Deserialize(ArrReader, DetArray))
				{
					TArray<FSomaPerceptionDetection> ParsedDetections = ParseDetectionArray(DetArray, FrameWidth, FrameHeight);
					OutSceneSummary.ObjectCount = ParsedDetections.Num();
					return ParsedDetections;
				}
			}

			UE_LOG(LogTemp, Warning, TEXT("soma_perception: OpenAI – could not extract detection array from model text: %s"),
				*Text.Left(256));
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("soma_perception: OpenAI – no output_text found in response"));
	return {};
}

TArray<FSomaPerceptionDetection> FOpenAiVisionClient::ParseDetectionArray(
	const TArray<TSharedPtr<FJsonValue>>& JsonArray,
	int32 FrameWidth, int32 FrameHeight)
{
	TArray<FSomaPerceptionDetection> Out;
	Out.Reserve(JsonArray.Num());

	for (const TSharedPtr<FJsonValue>& Elem : JsonArray)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Elem->TryGetObject(Obj) || !Obj)
		{
			continue;
		}

		FSomaPerceptionDetection Det;
		Det.Source = ESomaPerceptionDetectionSource::OpenAI;

		if (!(*Obj)->TryGetStringField(TEXT("class_name"), Det.ClassName))
		{
			continue;
		}

		double RawConf = 0.0;
		(*Obj)->TryGetNumberField(TEXT("confidence"), RawConf);
		Det.Confidence = FMath::Clamp(static_cast<float>(RawConf), 0.0f, 1.0f);
		(*Obj)->TryGetStringField(TEXT("description"), Det.Description);
		(*Obj)->TryGetStringField(TEXT("action"), Det.Action);

		const TArray<TSharedPtr<FJsonValue>>* AttributesArray = nullptr;
		if ((*Obj)->TryGetArrayField(TEXT("attributes"), AttributesArray) && AttributesArray)
		{
			for (const TSharedPtr<FJsonValue>& AttrVal : *AttributesArray)
			{
				FString AttrText;
				if (AttrVal.IsValid() && AttrVal->TryGetString(AttrText) && !AttrText.IsEmpty())
				{
					Det.Attributes.Add(AttrText);
				}
			}
		}

		auto ReadVec = [&](const FString& Key, FVector2D& OutVec) -> bool
		{
			const TSharedPtr<FJsonObject>* Sub = nullptr;
			if (!(*Obj)->TryGetObjectField(Key, Sub) || !Sub)
			{
				return false;
			}
			double X = 0.0, Y = 0.0;
			(*Sub)->TryGetNumberField(TEXT("x"), X);
			(*Sub)->TryGetNumberField(TEXT("y"), Y);
			OutVec.X = static_cast<float>(X);
			OutVec.Y = static_cast<float>(Y);
			return true;
		};

		if (!ReadVec(TEXT("bbox_min"), Det.BBoxMin) || !ReadVec(TEXT("bbox_max"), Det.BBoxMax))
		{
			continue;
		}

		// Clamp to frame bounds and reject degenerate boxes.
		Det.BBoxMin.X = FMath::Clamp(Det.BBoxMin.X, 0.0f, (float)FrameWidth);
		Det.BBoxMin.Y = FMath::Clamp(Det.BBoxMin.Y, 0.0f, (float)FrameHeight);
		Det.BBoxMax.X = FMath::Clamp(Det.BBoxMax.X, 0.0f, (float)FrameWidth);
		Det.BBoxMax.Y = FMath::Clamp(Det.BBoxMax.Y, 0.0f, (float)FrameHeight);

		if (Det.BBoxMax.X <= Det.BBoxMin.X || Det.BBoxMax.Y <= Det.BBoxMin.Y)
		{
			continue;
		}

		Det.ClassId = -1;
		Out.Add(MoveTemp(Det));
	}

	return Out;
}

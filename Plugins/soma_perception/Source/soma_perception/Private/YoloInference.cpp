#include "YoloInference.h"

#include "HAL/Platform.h"
#include "Misc/Char.h"
#include "Misc/Paths.h"

// COCO 80 class labels (Ultralytics default order)
static const TArray<FString> G_CocoClassNames = {
	TEXT("person"),
	TEXT("bicycle"),
	TEXT("car"),
	TEXT("motorcycle"),
	TEXT("airplane"),
	TEXT("bus"),
	TEXT("train"),
	TEXT("truck"),
	TEXT("boat"),
	TEXT("traffic light"),
	TEXT("fire hydrant"),
	TEXT("stop sign"),
	TEXT("parking meter"),
	TEXT("bench"),
	TEXT("bird"),
	TEXT("cat"),
	TEXT("dog"),
	TEXT("horse"),
	TEXT("sheep"),
	TEXT("cow"),
	TEXT("elephant"),
	TEXT("bear"),
	TEXT("zebra"),
	TEXT("giraffe"),
	TEXT("backpack"),
	TEXT("umbrella"),
	TEXT("handbag"),
	TEXT("tie"),
	TEXT("suitcase"),
	TEXT("frisbee"),
	TEXT("skis"),
	TEXT("snowboard"),
	TEXT("sports ball"),
	TEXT("kite"),
	TEXT("baseball bat"),
	TEXT("baseball glove"),
	TEXT("skateboard"),
	TEXT("surfboard"),
	TEXT("tennis racket"),
	TEXT("bottle"),
	TEXT("wine glass"),
	TEXT("cup"),
	TEXT("fork"),
	TEXT("knife"),
	TEXT("spoon"),
	TEXT("bowl"),
	TEXT("banana"),
	TEXT("apple"),
	TEXT("sandwich"),
	TEXT("orange"),
	TEXT("broccoli"),
	TEXT("carrot"),
	TEXT("hot dog"),
	TEXT("pizza"),
	TEXT("donut"),
	TEXT("cake"),
	TEXT("chair"),
	TEXT("couch"),
	TEXT("potted plant"),
	TEXT("bed"),
	TEXT("dining table"),
	TEXT("toilet"),
	TEXT("tv"),
	TEXT("laptop"),
	TEXT("mouse"),
	TEXT("remote"),
	TEXT("keyboard"),
	TEXT("cell phone"),
	TEXT("microwave"),
	TEXT("oven"),
	TEXT("toaster"),
	TEXT("sink"),
	TEXT("refrigerator"),
	TEXT("book"),
	TEXT("clock"),
	TEXT("vase"),
	TEXT("scissors"),
	TEXT("teddy bear"),
	TEXT("hair drier"),
	TEXT("toothbrush")
};

YoloInference::YoloInference()
{
	// ONNX Runtime global API entrypoint.
	const OrtApiBase* ApiBase = OrtGetApiBase();
	const char* RuntimeVersionC = ApiBase ? ApiBase->GetVersionString() : nullptr;
	int32 ResolvedApiVersion = ORT_API_VERSION;
	OrtApi = ApiBase ? ApiBase->GetApi(ORT_API_VERSION) : nullptr;

	// Some packaged runtimes may be older than our headers. Fall back to the
	// highest compatible API version to keep plugin init resilient.
	if (!OrtApi && ApiBase)
	{
		for (int32 ApiVersion = ORT_API_VERSION - 1; ApiVersion >= 1; --ApiVersion)
		{
			OrtApi = ApiBase->GetApi((uint32_t)ApiVersion);
			if (OrtApi)
			{
				ResolvedApiVersion = ApiVersion;
				break;
			}
		}
	}

}

YoloInference::~YoloInference()
{
	ReleaseSession();
}

bool YoloInference::LoadModel(const FString& OnnxPath)
{
	ReleaseSession();

	if (OnnxPath.IsEmpty())
	{
		return false;
	}

	if (!OrtApi)
	{
		return false;
	}

	// Create ORT environment.
	OrtStatus* Status = OrtApi->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "soma_perception", &OrtEnv);
	if (Status != nullptr || OrtEnv == nullptr)
	{
		return false;
	}

	// Session options.
	OrtApi->CreateSessionOptions(&OrtSessionOptions);
	OrtApi->SetSessionExecutionMode(OrtSessionOptions, ORT_SEQUENTIAL);

	// CPU memory info (for CreateTensorWithDataAsOrtValue).
	OrtApi->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &CpuMemoryInfo);

	// Create session from model file.
	// On Windows, ORTCHAR_T is wchar_t, which matches UE's TCHAR on Windows builds.
	const ORTCHAR_T* ModelPathPtr = reinterpret_cast<const ORTCHAR_T*>(*OnnxPath);
	Status = OrtApi->CreateSession(OrtEnv, ModelPathPtr, OrtSessionOptions, &OrtSession);
	if (Status != nullptr || OrtSession == nullptr)
	{
		return false;
	}

	// Allocator for name queries.
	Status = OrtApi->GetAllocatorWithDefaultOptions(&OrtAllocator);
	if (Status != nullptr || OrtAllocator == nullptr)
	{
		return false;
	}

	// Cache input/output names.
	size_t InputCount = 0;
	Status = OrtApi->SessionGetInputCount(OrtSession, &InputCount);
	if (Status != nullptr || InputCount < 1)
	{
		ReleaseSession();
		return false;
	}

	{
		char* InputNameC = nullptr;
		OrtApi->SessionGetInputName(OrtSession, 0, OrtAllocator, &InputNameC);
		if (InputNameC)
		{
			InputNameUtf8 = std::string(InputNameC);
			OrtApi->AllocatorFree(OrtAllocator, InputNameC);
		}
	}

	size_t OutputCount = 0;
	if (OrtApi->SessionGetOutputCount(OrtSession, &OutputCount) != nullptr || OutputCount < 1)
	{
		ReleaseSession();
		return false;
	}

	OutputNamesUtf8.Empty(OutputCount);
	for (size_t i = 0; i < OutputCount; ++i)
	{
		char* OutputNameC = nullptr;
		OrtApi->SessionGetOutputName(OrtSession, i, OrtAllocator, &OutputNameC);
		if (OutputNameC)
		{
			OutputNamesUtf8.Add(std::string(OutputNameC));
			OrtApi->AllocatorFree(OrtAllocator, OutputNameC);
		}
	}

	bLoaded = true;
	return true;
}

void YoloInference::ReleaseSession()
{
	if (!OrtApi)
	{
		return;
	}

	bLoaded = false;

	if (OrtSessionOptions)
	{
		OrtApi->ReleaseSessionOptions(OrtSessionOptions);
		OrtSessionOptions = nullptr;
	}
	if (OrtSession)
	{
		OrtApi->ReleaseSession(OrtSession);
		OrtSession = nullptr;
	}
	if (OrtEnv)
	{
		OrtApi->ReleaseEnv(OrtEnv);
		OrtEnv = nullptr;
	}

	if (CpuMemoryInfo)
	{
		OrtApi->ReleaseMemoryInfo(CpuMemoryInfo);
		CpuMemoryInfo = nullptr;
	}

	if (OrtAllocator)
	{
		// Allocator from GetAllocatorWithDefaultOptions is owned by ORT runtime.
		// Releasing it can crash during teardown.
		OrtAllocator = nullptr;
	}

	InputNameUtf8.clear();
	OutputNamesUtf8.Empty();
}

bool YoloInference::Preprocess(
	const TArray<uint8>& RGBPixels,
	int32 Width,
	int32 Height,
	TArray<float>& OutTensorNCHW) const
{
	// Model input: NCHW float32, 1x3x640x640.
	const int32 InW = InputSize;
	const int32 InH = InputSize;
	const int64 TotalFloats = 1LL * 3LL * InW * InH;
	OutTensorNCHW.SetNumUninitialized((int32)TotalFloats);

	if (Width <= 0 || Height <= 0)
	{
		return false;
	}

	// Simple resize (nearest-neighbor) from original viewport to 640x640.
	for (int32 y = 0; y < InH; ++y)
	{
		int32 SrcY = FMath::Clamp((int32)FMath::FloorToInt((float)y * (float)Height / (float)InH), 0, Height - 1);
		for (int32 x = 0; x < InW; ++x)
		{
			int32 SrcX = FMath::Clamp((int32)FMath::FloorToInt((float)x * (float)Width / (float)InW), 0, Width - 1);
			const int32 PixIndex = (SrcY * Width + SrcX) * 3;

			// RGBPixels is expected in RGB order already.
			const float R = (float)RGBPixels[PixIndex + 0] / 255.0f;
			const float G = (float)RGBPixels[PixIndex + 1] / 255.0f;
			const float B = (float)RGBPixels[PixIndex + 2] / 255.0f;

			const int64 HW = 1LL * InW * InH;
			OutTensorNCHW[(int32)(0 * HW + (int64)y * (int64)InW + x)] = R;
			OutTensorNCHW[(int32)(1 * HW + (int64)y * (int64)InW + x)] = G;
			OutTensorNCHW[(int32)(2 * HW + (int64)y * (int64)InW + x)] = B;
		}
	}

	return true;
}

TArray<FSomaPerceptionDetection> YoloInference::RunInference(
	const TArray<uint8>& RGBPixels,
	int32 Width,
	int32 Height,
	float ConfThresh,
	float NMSThresh)
{
	TArray<FSomaPerceptionDetection> Empty;

	if (!bLoaded || !OrtApi || !OrtSession || !CpuMemoryInfo)
	{
		return Empty;
	}

	if (RGBPixels.Num() < Width * Height * 3)
	{
		return Empty;
	}

	TArray<float> TensorNCHW;
	if (!Preprocess(RGBPixels, Width, Height, TensorNCHW))
	{
		return Empty;
	}

	// Create input tensor (backed by TensorNCHW memory).
	const int64_t Shape[4] = { 1, 3, InputSize, InputSize };
	const int32 NumElements = 1 * 3 * InputSize * InputSize;
	const size_t ByteSize = (size_t)(NumElements * sizeof(float));

	OrtValue* InputTensor = nullptr;
	void* TensorDataPtr = TensorNCHW.GetData();
	if (OrtApi->CreateTensorWithDataAsOrtValue(CpuMemoryInfo, TensorDataPtr, ByteSize, Shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &InputTensor) != nullptr)
	{
		return Empty;
	}

	// Run.
	const char* InputNameC = InputNameUtf8.empty() ? nullptr : InputNameUtf8.c_str();
	TArray<const char*> OutputNamesC;
	OutputNamesC.Reserve(OutputNamesUtf8.Num());
	for (const std::string& S : OutputNamesUtf8)
	{
		OutputNamesC.Add(S.c_str());
	}
	TArray<OrtValue*> OutputValues;
	const size_t OutputCount = (size_t)OutputNamesC.Num();
	OutputValues.SetNumZeroed((int32)OutputCount);

	// OrtApi::Run signature wants:
	// - input_names: array of null-terminated UTF-8 strings => const char* const*
	// - input tensors: array of OrtValue => const OrtValue* const*
	// - output names: const char* const*
	// - outputs: OrtValue** (can be nullptr to allocate; we pass non-null array pointers)
	// Some ORT versions treat outputs[] entries as in/out, so we pass the addresses of our slots.
	{
		const char* const* OutputNamesArr = OutputNamesC.Num() > 0
			? reinterpret_cast<const char* const*>(OutputNamesC.GetData())
			: nullptr;
		if (OutputNamesC.Num() == 0 || InputNameC == nullptr)
		{
			OrtApi->ReleaseValue(InputTensor);
			return Empty;
		}

		// Allocate outputs on ORT side by passing nullptr entries (here we pass our array initialized with nullptr).
		OrtValue** OutputValuesPtr = OutputValues.GetData();
		const char* const InputNamesArr[1] = { InputNameC };
		const OrtValue* InputTensorsArr[1] = { InputTensor };
		if (OrtApi->Run(OrtSession, nullptr, InputNamesArr, InputTensorsArr, 1, OutputNamesArr, OutputCount, OutputValuesPtr) != nullptr)
		{
			OrtApi->ReleaseValue(InputTensor);
			return Empty;
		}
	}

	// Decode first output tensor.
	if (OutputValues.Num() < 1 || !OutputValues[0])
	{
		for (OrtValue* V : OutputValues)
		{
			if (V)
			{
				OrtApi->ReleaseValue(V);
			}
		}
		OrtApi->ReleaseValue(InputTensor);
		return Empty;
	}

	OrtValue* Output0 = OutputValues[0];
	OrtTensorTypeAndShapeInfo* ShapeInfo = nullptr;
	if (OrtApi->GetTensorTypeAndShape(Output0, &ShapeInfo) != nullptr || !ShapeInfo)
	{
		for (OrtValue* V : OutputValues)
		{
			if (V)
			{
				OrtApi->ReleaseValue(V);
			}
		}
		OrtApi->ReleaseValue(InputTensor);
		return Empty;
	}

	size_t DimCount = 0;
	OrtApi->GetDimensionsCount(ShapeInfo, &DimCount);
	TArray<int64_t> OutputShape;
	OutputShape.SetNum((int32)DimCount);
	if (DimCount > 0)
	{
		TArray<int64_t> DimsTmp;
		DimsTmp.SetNum((int32)DimCount);
		OrtApi->GetDimensions(ShapeInfo, DimsTmp.GetData(), DimCount);
		for (size_t i = 0; i < DimCount; ++i)
		{
			OutputShape[(int32)i] = DimsTmp[(int32)i];
		}
	}

	// Read output tensor data as float.
	void* OutputDataPtr = nullptr;
	OrtApi->GetTensorMutableData(Output0, &OutputDataPtr);
	const float* OutputData = (const float*)OutputDataPtr;
	if (!OutputData)
	{
		for (OrtValue* V : OutputValues)
		{
			if (V)
			{
				OrtApi->ReleaseValue(V);
			}
		}
		OrtApi->ReleaseValue(InputTensor);
		return Empty;
	}

	TArray<FSomaPerceptionDetection> Detections = DecodeAndNms(OutputData, OutputShape, Width, Height, ConfThresh, NMSThresh);

	OrtApi->ReleaseTensorTypeAndShapeInfo(ShapeInfo);

	for (OrtValue* V : OutputValues)
	{
		if (V)
		{
			OrtApi->ReleaseValue(V);
		}
	}
	OrtApi->ReleaseValue(InputTensor);

	return Detections;
}

TArray<FSomaPerceptionDetection> YoloInference::DecodeAndNms(
	const float* OutputData,
	const TArray<int64_t>& OutputShape,
	int32 OrigWidth,
	int32 OrigHeight,
	float ConfThresh,
	float NMSThresh) const
{
	TArray<FSomaPerceptionDetection> RawDetections;

	if (!OutputData || OutputShape.Num() < 2)
	{
		return RawDetections;
	}

	// Output shape is expected to be one of:
	// [1, N, C] or [1, C, N] where C is 4+num_classes or 5+num_classes.
	// We support both layouts.
	bool bChannelFirst = false;
	int32 NumBoxes = 0;
	int32 FeatureLen = 0;

	auto TrySetLayout = [&](int32 d1, int32 d2)
	{
		const int32 ExpectedA = 4 + NumClasses;
		const int32 ExpectedB = 5 + NumClasses;
		if (d1 == ExpectedA || d1 == ExpectedB)
		{
			// [1, C, N]
			bChannelFirst = true;
			FeatureLen = d1;
			NumBoxes = d2;
			return true;
		}
		if (d2 == ExpectedA || d2 == ExpectedB)
		{
			// [1, N, C]
			bChannelFirst = false;
			FeatureLen = d2;
			NumBoxes = d1;
			return true;
		}
		return false;
	};

	if (OutputShape.Num() == 3)
	{
		const int32 d1 = (int32)OutputShape[1];
		const int32 d2 = (int32)OutputShape[2];
		if (!TrySetLayout(d1, d2))
		{
			// Fallback: assume [1, N, C].
			bChannelFirst = false;
			NumBoxes = d1;
			FeatureLen = d2;
		}
	}
	else if (OutputShape.Num() == 2)
	{
		// [N, C]
		bChannelFirst = false;
		NumBoxes = (int32)OutputShape[0];
		FeatureLen = (int32)OutputShape[1];
	}
	else
	{
		// Unsupported rank.
		return RawDetections;
	}

	if (NumBoxes <= 0 || FeatureLen < 4)
	{
		return RawDetections;
	}

	const int32 ExpectedNoObjLen = 4 + NumClasses; // 4 + classes
	const int32 ExpectedWithObjLen = 5 + NumClasses; // 5 + classes
	const bool bHasObjectness = (FeatureLen == ExpectedWithObjLen);

	auto Get = [&](int32 ChannelOrFeature, int32 BoxIndex) -> float
	{
		if (bChannelFirst)
		{
			// [1, C, N]
			return OutputData[(int64)ChannelOrFeature * (int64)NumBoxes + (int64)BoxIndex];
		}
		// [1, N, C]
		return OutputData[(int64)BoxIndex * (int64)FeatureLen + (int64)ChannelOrFeature];
	};

	const int32 CoordIdxX = 0;
	const int32 CoordIdxY = 1;
	const int32 CoordIdxW = 2;
	const int32 CoordIdxH = 3;
	const int32 ObjIdx = 4;
	const int32 ClassStartIdx = bHasObjectness ? 5 : 4;

	// Decode candidates.
	for (int32 i = 0; i < NumBoxes; ++i)
	{
		float x = Get(CoordIdxX, i);
		float y = Get(CoordIdxY, i);
		float w = Get(CoordIdxW, i);
		float h = Get(CoordIdxH, i);

		// Heuristic: if values look like normalized coordinates (0..1), scale up to 0..InputSize.
		if (FMath::Abs(x) <= 2.0f && FMath::Abs(y) <= 2.0f && FMath::Abs(w) <= 2.0f && FMath::Abs(h) <= 2.0f)
		{
			x *= (float)InputSize;
			y *= (float)InputSize;
			w *= (float)InputSize;
			h *= (float)InputSize;
		}

		float Obj = 1.0f;
		if (bHasObjectness)
		{
			Obj = Get(ObjIdx, i);
		}

		int32 BestClassId = -1;
		float BestClassScore = -FLT_MAX;
		for (int32 c = 0; c < NumClasses; ++c)
		{
			const float Score = Get(ClassStartIdx + c, i);
			if (Score > BestClassScore)
			{
				BestClassScore = Score;
				BestClassId = c;
			}
		}

		const float Confidence = Obj * BestClassScore;
		if (Confidence < ConfThresh || BestClassId < 0)
		{
			continue;
		}

		const float Left = x - w * 0.5f;
		const float Top = y - h * 0.5f;
		const float Right = x + w * 0.5f;
		const float Bottom = y + h * 0.5f;

		const float ClampedLeft = FMath::Clamp(Left, 0.0f, (float)InputSize);
		const float ClampedTop = FMath::Clamp(Top, 0.0f, (float)InputSize);
		const float ClampedRight = FMath::Clamp(Right, 0.0f, (float)InputSize);
		const float ClampedBottom = FMath::Clamp(Bottom, 0.0f, (float)InputSize);

		// Map 640x640 coordinates back to the original viewport.
		const float ScaleX = (float)OrigWidth / (float)InputSize;
		const float ScaleY = (float)OrigHeight / (float)InputSize;

		FSomaPerceptionDetection Det;
		Det.ClassId = BestClassId;
		Det.ClassName = G_CocoClassNames.IsValidIndex(BestClassId) ? G_CocoClassNames[BestClassId] : FString(TEXT("unknown"));
		Det.Confidence = Confidence;
		Det.BBoxMin = FVector2D(ClampedLeft * ScaleX, ClampedTop * ScaleY);
		Det.BBoxMax = FVector2D(ClampedRight * ScaleX, ClampedBottom * ScaleY);

		RawDetections.Add(Det);
	}

	if (RawDetections.Num() == 0)
	{
		return RawDetections;
	}

	// Run per-class NMS.
	TArray<FSomaPerceptionDetection> Final;
	Final.Reserve(RawDetections.Num());

	for (int32 classId = 0; classId < NumClasses; ++classId)
	{
		TArray<FSomaPerceptionDetection> ClassCands;
		for (const FSomaPerceptionDetection& d : RawDetections)
		{
			if (d.ClassId == classId)
			{
				ClassCands.Add(d);
			}
		}

		if (ClassCands.Num() == 0)
		{
			continue;
		}

		ClassCands.Sort([](const FSomaPerceptionDetection& A, const FSomaPerceptionDetection& B)
		{
			return A.Confidence > B.Confidence;
		});

		const int32 CandCount = ClassCands.Num();
		TArray<bool> bSuppressed;
		bSuppressed.Init(false, CandCount);

		for (int32 i = 0; i < CandCount; ++i)
		{
			if (bSuppressed[i])
			{
				continue;
			}

			const FSomaPerceptionDetection& Best = ClassCands[i];
			Final.Add(Best);

			for (int32 j = i + 1; j < CandCount; ++j)
			{
				if (bSuppressed[j])
				{
					continue;
				}

				float IoU = 0.0f;
				GetIou(Best, ClassCands[j], IoU);
				if (IoU > NMSThresh)
				{
					bSuppressed[j] = true;
				}
			}
		}
	}

	return Final;
}

void YoloInference::GetIou(const FSomaPerceptionDetection& A, const FSomaPerceptionDetection& B, float& OutIou)
{
	const float InterLeft = FMath::Max(A.BBoxMin.X, B.BBoxMin.X);
	const float InterTop = FMath::Max(A.BBoxMin.Y, B.BBoxMin.Y);
	const float InterRight = FMath::Min(A.BBoxMax.X, B.BBoxMax.X);
	const float InterBottom = FMath::Min(A.BBoxMax.Y, B.BBoxMax.Y);

	const float InterW = FMath::Max(0.0f, InterRight - InterLeft);
	const float InterH = FMath::Max(0.0f, InterBottom - InterTop);
	const float InterArea = InterW * InterH;

	const float AreaA = FMath::Max(0.0f, A.BBoxMax.X - A.BBoxMin.X) * FMath::Max(0.0f, A.BBoxMax.Y - A.BBoxMin.Y);
	const float AreaB = FMath::Max(0.0f, B.BBoxMax.X - B.BBoxMin.X) * FMath::Max(0.0f, B.BBoxMax.Y - B.BBoxMin.Y);

	OutIou = InterArea / (AreaA + AreaB - InterArea + KINDA_SMALL_NUMBER);
}

const TArray<FString>& YoloInference::GetCocoClassNames()
{
	return G_CocoClassNames;
}


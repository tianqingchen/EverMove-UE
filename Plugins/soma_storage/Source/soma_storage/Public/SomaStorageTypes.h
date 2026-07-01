#pragma once

#include "CoreMinimal.h"
#include "SomaStorageTypes.generated.h"

// ---------------------------------------------------------------------------
// Perception data shapes
//
// These structs describe what the perception layer sees. They live here, in
// soma_storage, so that USomaStorageSubsystem can accept them directly without
// pulling in a circular dependency on soma_perception. soma_perception (and
// any other consumer) depends on soma_storage and uses these types.
// ---------------------------------------------------------------------------

UENUM(BlueprintType)
enum class ESomaPerceptionDetectionSource : uint8
{
	YOLO   UMETA(DisplayName = "YOLO (Local)"),
	OpenAI UMETA(DisplayName = "OpenAI (Cloud)")
};

USTRUCT(BlueprintType)
struct SOMA_STORAGE_API FSomaPerceptionDetection
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SomaPerception")
	FString ClassName;

	UPROPERTY(BlueprintReadOnly, Category = "SomaPerception")
	int32 ClassId = -1;

	UPROPERTY(BlueprintReadOnly, Category = "SomaPerception")
	float Confidence = 0.0f;

	/** Top-left corner (viewport/screen-space pixels). */
	UPROPERTY(BlueprintReadOnly, Category = "SomaPerception")
	FVector2D BBoxMin = FVector2D::ZeroVector;

	/** Bottom-right corner (viewport/screen-space pixels). */
	UPROPERTY(BlueprintReadOnly, Category = "SomaPerception")
	FVector2D BBoxMax = FVector2D::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "SomaPerception")
	ESomaPerceptionDetectionSource Source = ESomaPerceptionDetectionSource::YOLO;

	/** Round-trip latency in seconds for the inference that produced this detection. */
	UPROPERTY(BlueprintReadOnly, Category = "SomaPerception")
	float LatencySeconds = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "SomaPerception")
	FString Description;

	UPROPERTY(BlueprintReadOnly, Category = "SomaPerception")
	TArray<FString> Attributes;

	UPROPERTY(BlueprintReadOnly, Category = "SomaPerception")
	FString Action;
};

USTRUCT(BlueprintType)
struct SOMA_STORAGE_API FSomaPerceptionSceneSummary
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SomaPerception")
	FString Summary;

	UPROPERTY(BlueprintReadOnly, Category = "SomaPerception")
	int32 ObjectCount = 0;
};

/**
 * A single gaze-able world object. The designer-assigned `Name` is the only thing
 * the LLM ever sees / returns; `Position` is the authoritative live world location
 * resolved engine-side so the model can never hallucinate coordinates.
 */
USTRUCT(BlueprintType)
struct SOMA_STORAGE_API FSomaGazeCandidate
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "SomaPerception")
	FString Name;

	UPROPERTY(BlueprintReadWrite, Category = "SomaPerception")
	FVector Position = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "SomaPerception")
	FString Description;
};

// ---------------------------------------------------------------------------
// Storage entry types
// ---------------------------------------------------------------------------

UENUM(BlueprintType)
enum class ESomaPerceptionEntrySource : uint8
{
	Vision UMETA(DisplayName = "Vision"),
	ASR    UMETA(DisplayName = "ASR"),
	Custom UMETA(DisplayName = "Custom")
};

USTRUCT(BlueprintType)
struct SOMA_STORAGE_API FSomaPerceptionEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SomaStorage")
	FDateTime Timestamp;

	UPROPERTY(BlueprintReadOnly, Category = "SomaStorage")
	ESomaPerceptionEntrySource Source = ESomaPerceptionEntrySource::Vision;

	/** Rendered, LLM-ready block (timestamp + summary + per-object lines for vision; transcript text for ASR). */
	UPROPERTY(BlueprintReadOnly, Category = "SomaStorage")
	FString Text;

	/** Cached whitespace-split word count of `Text`. */
	UPROPERTY(BlueprintReadOnly, Category = "SomaStorage")
	int32 WordCount = 0;
};

// ---------------------------------------------------------------------------
// Structured prompt types
// ---------------------------------------------------------------------------

USTRUCT(BlueprintType)
struct SOMA_STORAGE_API FSomaActionMapping
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaStorage")
	FString Tag;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaStorage")
	FString Event;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaStorage")
	FString Description;
};

USTRUCT(BlueprintType)
struct SOMA_STORAGE_API FSomaPromptConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaStorage")
	FString CharacterName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaStorage")
	FString CharacterDescription;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaStorage")
	FString CharacterGoal;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SomaStorage")
	TArray<FSomaActionMapping> AvailableActions;
};

USTRUCT(BlueprintType)
struct SOMA_STORAGE_API FSomaStructuredResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SomaStorage")
	FString Reasoning;

	UPROPERTY(BlueprintReadOnly, Category = "SomaStorage")
	FString Dialogue;

	UPROPERTY(BlueprintReadOnly, Category = "SomaStorage")
	FString ActionTag;

	UPROPERTY(BlueprintReadOnly, Category = "SomaStorage")
	FString ActionEvent;

	/** Name of the chosen gaze object (exactly as listed under Visible Objects), or empty if none. */
	UPROPERTY(BlueprintReadOnly, Category = "SomaStorage")
	FString GazeObject;

	/** Authoritative world position resolved engine-side from the live gaze registry. */
	UPROPERTY(BlueprintReadOnly, Category = "SomaStorage")
	FVector GazePosition = FVector::ZeroVector;

	/** True when the LLM selected a non-empty gaze object. */
	UPROPERTY(BlueprintReadOnly, Category = "SomaStorage")
	bool bHasGazeTarget = false;
};

// ---------------------------------------------------------------------------
// Delegates
// ---------------------------------------------------------------------------

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSomaStorageOnDynamicLogChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSomaStorageOnPersistentChanged, const FString&, NewText);

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimSequence.h"
#include "SomaLipsyncTypes.generated.h"

/**
 * 15 OVR-style visemes emitted by ASomaVoiceChatbot::OnVisemeFrame.
 * Index order MUST match the float array in FSomaVisemeFrameData::VisemeScores
 * and the CharToVisemeIndex table in SomaVoiceChatbot.cpp.
 */
UENUM(BlueprintType)
enum class ESomaLipsyncViseme : uint8
{
	Sil = 0,
	PP,
	FF,
	TH,
	DD,
	KK,
	CH,
	SS,
	NN,
	RR,
	AA,
	EH,
	IH,
	OH,
	OU,
	MAX UMETA(Hidden)
};

/** Local-bone-space target pose for the 4 mouth bones. Parent-relative per PDF section 4.1. */
USTRUCT(BlueprintType)
struct SOMA_LIPSYNC_API FLipsyncMouthPose
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync")
	FTransform LowLip = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync")
	FTransform UpLip = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync")
	FTransform LeftCorner = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync")
	FTransform RightCorner = FTransform::Identity;
};

/**
 * Trajectory-matching station offsets shared by the baker and the runtime
 * matcher. Order: [past, now, future1, future2, future3].
 */
namespace SomaLipsyncTraj
{
	static constexpr int32 NumStations = 5;
	static constexpr float StationOffsetsSeconds[NumStations] =
		{ -0.100f, 0.0f, 0.067f, 0.133f, 0.200f };
}

/**
 * A short trajectory of mouth poses sampled around a center time, used by both
 * the database baker (per-sample) and the runtime feature builder (per-tick).
 * Stations is laid out per SomaLipsyncTraj::StationOffsetsSeconds.
 *
 * bValid=false means this window could not be filled (e.g. station offset fell
 * outside the source clip during baking). The matcher then falls back to the
 * single-pose cost so legacy / un-rebuilt databases keep working.
 */
USTRUCT(BlueprintType)
struct SOMA_LIPSYNC_API FLipsyncPoseWindow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync")
	TArray<FLipsyncMouthPose> Stations;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync")
	bool bValid = false;
};

/** One indexed sample inside ULipsyncMotionDatabase. */
USTRUCT(BlueprintType)
struct SOMA_LIPSYNC_API FLipsyncPoseSample
{
	GENERATED_BODY()

	/** Source clip the sample was taken from. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync")
	TObjectPtr<UAnimSequence> Sequence = nullptr;

	/** Sample time within Sequence, in seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync")
	float Time = 0.f;

	/** Captured 4-bone local-space mouth pose (legacy single pose; still used as the 'now' station fallback). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync")
	FLipsyncMouthPose MouthPose;

	/**
	 * Trajectory window of mouth poses sampled at SomaLipsyncTraj station
	 * offsets around Time. bValid=false on samples baked before trajectory
	 * matching was added, or whose offsets fall outside the source clip; the
	 * matcher falls back to MouthPose in that case.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync")
	FLipsyncPoseWindow Window;

	/** Optional human-readable label (e.g. asset name) for debug overlay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync")
	FString SourceLabel;
};

/** Result of a brute-force search. Consumed by AnimBP and debug overlay. */
USTRUCT(BlueprintType)
struct SOMA_LIPSYNC_API FLipsyncMatchResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Lipsync")
	TObjectPtr<UAnimSequence> SelectedSequence = nullptr;

	UPROPERTY(BlueprintReadWrite, Category = "Lipsync")
	float SelectedTime = 0.f;

	UPROPERTY(BlueprintReadWrite, Category = "Lipsync")
	float MatchCost = TNumericLimits<float>::Max();

	UPROPERTY(BlueprintReadWrite, Category = "Lipsync")
	FString SelectedLabel;

	UPROPERTY(BlueprintReadWrite, Category = "Lipsync")
	bool bValid = false;
};

namespace SomaLipsyncBones
{
	static const FName LowLip   = TEXT("lowlip_8_JNT");
	static const FName UpLip    = TEXT("uplip_9_JNT");
	static const FName LCorner  = TEXT("L_lipcorner_main_JNT");
	static const FName RCorner  = TEXT("R_lipcorner_main_JNT");
}

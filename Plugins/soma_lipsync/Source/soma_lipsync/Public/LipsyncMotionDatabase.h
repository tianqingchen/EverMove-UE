#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SomaLipsyncTypes.h"
#include "LipsyncMotionDatabase.generated.h"

class UAnimSequence;

/**
 * Indexed pool of facial animation samples. Each entry is a parent-relative
 * 4-bone mouth pose taken from one of SourceSequences at SampleRate fps.
 *
 * Use the BuildIndex editor button to populate Samples after editing the
 * source list.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Lipsync Motion Database"))
class SOMA_LIPSYNC_API ULipsyncMotionDatabase : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Animation sequences the index is built from. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Sources")
	TArray<TObjectPtr<UAnimSequence>> SourceSequences;

	/** Sampling rate used by BuildIndex (samples per second). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Sources", meta = (ClampMin = "1.0"))
	float SampleRate = 30.f;

	/** Bones extracted into each sample. Defaults to the 4 mouth bones. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Sources")
	TArray<FName> RequiredBones;

	/**
	 * Indexed samples. Built by BuildIndex.
	 *
	 * Intentionally NOT marked VisibleAnywhere/EditAnywhere: with a typical
	 * facial-animation pool, Samples ends up with ~hundreds of entries, each
	 * holding 6 mouth poses (1 single + 5 trajectory stations) of 4 FTransform
	 * fields. The Details panel materializes Slate widgets for every leaf
	 * (X/Y/Z floats of every translation/rotation/scale), which can balloon to
	 * 100k+ widgets and tens of GB of editor memory just to *view* the asset.
	 * The array is still a UPROPERTY so it serializes normally; expose the
	 * NumSamples / NumValidTrajectoryWindows summary below for editor feedback.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Lipsync|Output")
	TArray<FLipsyncPoseSample> Samples;

	/** Number of samples populated by the last BuildIndex (cached for the Details panel). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Lipsync|Output")
	int32 NumSamples = 0;

	/** Subset of NumSamples whose trajectory Window is fully baked (bValid + 5 stations). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Lipsync|Output")
	int32 NumValidTrajectoryWindows = 0;

	ULipsyncMotionDatabase();

	virtual void PostLoad() override;

	/** Recomputes the Details-panel summary fields (NumSamples, NumValidTrajectoryWindows) from Samples. */
	void RefreshOutputSummary();

#if WITH_EDITOR
	/** Editor-only: walks SourceSequences and rebuilds Samples using FSomaLipsyncBoneExtractor. */
	UFUNCTION(CallInEditor, Category = "Lipsync|Build")
	void BuildIndex();
#endif
};

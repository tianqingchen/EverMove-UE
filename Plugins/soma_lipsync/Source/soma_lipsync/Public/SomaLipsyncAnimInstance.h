#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "SomaLipsyncTypes.h"
#include "SomaLipsyncAnimInstance.generated.h"

class UAnimSequence;
class USomaLipsyncMotionMatcherComponent;

/**
 * Native AnimInstance base class for ABP_LipsyncMM.
 *
 * Cache the matcher in NativeInitializeAnimation, refresh the exposed match
 * fields in NativeUpdateAnimation, and let a Blueprint child wire the actual
 * AnimGraph (Sequence Evaluator with Set Sequence + Set Explicit Time, then
 * Layered Blend Per Bone onto the mouth bone branch driven by
 * SmoothedLipsyncBlendWeight).
 */
UCLASS(BlueprintType, Blueprintable, meta = (DisplayName = "Soma Lipsync Anim Instance"))
class SOMA_LIPSYNC_API USomaLipsyncAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	/** AnimGraph: feed this into the Sequence Evaluator's Sequence pin. */
	UPROPERTY(BlueprintReadOnly, Category = "Lipsync")
	TObjectPtr<UAnimSequence> SelectedLipsyncSequence = nullptr;

	/** AnimGraph: feed this into the Sequence Evaluator's Explicit Time pin. */
	UPROPERTY(BlueprintReadOnly, Category = "Lipsync")
	float SelectedLipsyncTime = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Lipsync")
	float SelectedMatchCost = 0.f;

	/** True when the matcher has produced a valid match this frame. */
	UPROPERTY(BlueprintReadOnly, Category = "Lipsync")
	bool bHasValidLipsyncMatch = false;

	/** Smoothed 0/1 blend weight: drive Layered Blend Per Bone with this. */
	UPROPERTY(BlueprintReadOnly, Category = "Lipsync")
	float SmoothedLipsyncBlendWeight = 0.f;

	/** Time constant for FInterpTo smoothing of the lipsync layer (seconds). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lipsync", meta = (ClampMin = "0.0"))
	float BlendInterpSpeed = 20.f;

	/** Current viseme (informational; useful for debug widgets in the AnimBP). */
	UPROPERTY(BlueprintReadOnly, Category = "Lipsync")
	ESomaLipsyncViseme CurrentLipsyncViseme = ESomaLipsyncViseme::Sil;

protected:
	virtual void NativeInitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

private:
	UPROPERTY(Transient)
	TWeakObjectPtr<USomaLipsyncMotionMatcherComponent> CachedMatcher;
};

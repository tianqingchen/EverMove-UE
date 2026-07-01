#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SomaLipsyncTypes.h"
#include "VisemePoseTargetMap.generated.h"

class UAnimSequence;

/**
 * Maps each ESomaLipsyncViseme to:
 *   - a source UAnimSequence the user authored / imported (one per viseme)
 *   - a captured FLipsyncMouthPose extracted from that sequence at CaptureTime
 *
 * Use the CaptureFromSourceAnims editor button to bake the captured map after
 * filling in SourceVisemeAnims.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Viseme Pose Target Map"))
class SOMA_LIPSYNC_API UVisemePoseTargetMap : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Source per-viseme animation assets. The user wires these in the editor. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Sources")
	TMap<ESomaLipsyncViseme, TObjectPtr<UAnimSequence>> SourceVisemeAnims;

	/** Default time within each source clip to sample (seconds). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Sources", meta = (ClampMin = "0.0"))
	float CaptureTime = 0.f;

	/** Optional per-viseme override of CaptureTime. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lipsync|Sources")
	TMap<ESomaLipsyncViseme, float> CaptureTimeOverrides;

	/** Captured local-space mouth poses. Filled by CaptureFromSourceAnims. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lipsync|Output")
	TMap<ESomaLipsyncViseme, FLipsyncMouthPose> Map;

	/** Returns the target mouth pose for Viseme. Falls back to Sil if missing. */
	UFUNCTION(BlueprintCallable, Category = "Lipsync")
	bool GetPose(ESomaLipsyncViseme Viseme, FLipsyncMouthPose& OutPose) const;

#if WITH_EDITOR
	/** Editor-only: walks SourceVisemeAnims and bakes Map using FSomaLipsyncBoneExtractor. */
	UFUNCTION(CallInEditor, Category = "Lipsync|Build")
	void CaptureFromSourceAnims();
#endif
};

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SomaGazeTargetComponent.generated.h"

/**
 * USomaGazeTargetComponent
 *
 * Drop this on any world actor the character is allowed to look at. The designer
 * assigns a human-readable `GazeObjectName` (the only thing the LLM ever sees /
 * returns); the live world position is always read from the owning actor, so the
 * model can never hallucinate coordinates.
 *
 * Each component self-registers into a per-world static registry on register and
 * removes itself on unregister. The perception collector walks that registry each
 * gaze tick to build the candidate snapshot for soma_storage.
 */
UCLASS(ClassGroup = (Soma), meta = (BlueprintSpawnableComponent))
class SOMA_PERCEPTION_API USomaGazeTargetComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USomaGazeTargetComponent();

	/** Name the LLM sees and returns. Must be unique enough to disambiguate among Visible Objects. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Gaze")
	FString GazeObjectName;

	/** Optional short hint shown to the LLM alongside the name (e.g. "the locked exit"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Soma Gaze")
	FString Description;

	/** Live world location used as the authoritative gaze position. */
	UFUNCTION(BlueprintPure, Category = "Soma Gaze")
	FVector GetGazeWorldPosition() const;

	/** Returns all registered gaze targets that belong to `World` (skips pending-kill components). */
	static void GetGazeTargetsForWorld(const UWorld* World, TArray<USomaGazeTargetComponent*>& OutComponents);

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

private:
	static TArray<TWeakObjectPtr<USomaGazeTargetComponent>> Registry;
};

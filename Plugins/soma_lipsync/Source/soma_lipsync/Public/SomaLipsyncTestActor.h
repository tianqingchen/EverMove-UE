#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SomaLipsyncTypes.h"
#include "SomaLipsyncTestActor.generated.h"

class USkeletalMeshComponent;
class USomaLipsyncMotionMatcherComponent;

/**
 * Drop-in test harness: bind to keys 1..= to cycle the 15 visemes manually
 * before audio is wired. Subclass it as a Blueprint (BP_LipsyncTestActor) to
 * pick a Skeletal Mesh + the ABP_LipsyncMM AnimInstance.
 *
 * Default key bindings (Auto Receive Input = Player 0):
 *   1 = Sil   2 = PP   3 = FF   4 = TH   5 = DD
 *   6 = KK   7 = CH   8 = SS   9 = NN   0 = RR
 *   - = AA   = = EH   [ = IH   ] = OH   \\ = OU
 */
UCLASS(BlueprintType, Blueprintable, meta = (DisplayName = "Soma Lipsync Test Actor"))
class SOMA_LIPSYNC_API ASomaLipsyncTestActor : public AActor
{
	GENERATED_BODY()

public:
	ASomaLipsyncTestActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USkeletalMeshComponent> SkeletalMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USomaLipsyncMotionMatcherComponent> Matcher;

	/** If true, BeginPlay calls EnableInput on player 0 and binds the viseme keys. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Test")
	bool bAutoBindInput = true;

	UFUNCTION(BlueprintCallable, Category = "Lipsync|Test")
	void TriggerViseme(ESomaLipsyncViseme Viseme);

protected:
	virtual void BeginPlay() override;
};

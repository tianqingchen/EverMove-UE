#include "SomaLipsyncAnimInstance.h"

#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"

#include "SomaLipsyncMatcher.h"
#include "SomaLipsyncModule.h"

namespace
{
	USomaLipsyncMotionMatcherComponent* FindMatcherForAnimInstance(USomaLipsyncAnimInstance* AnimInstance)
	{
		if (!AnimInstance)
		{
			return nullptr;
		}

		// Prefer the pawn owner (e.g. a Character with the matcher component).
		if (APawn* Pawn = AnimInstance->TryGetPawnOwner())
		{
			if (USomaLipsyncMotionMatcherComponent* OnPawn = Pawn->FindComponentByClass<USomaLipsyncMotionMatcherComponent>())
			{
				return OnPawn;
			}
		}

		// Fall back to the actor that owns the SkeletalMeshComponent driving this
		// AnimInstance. This covers ASomaLipsyncTestActor (plain AActor) and any
		// other non-pawn carrier of the matcher.
		if (AActor* OwnerActor = AnimInstance->GetOwningActor())
		{
			if (USomaLipsyncMotionMatcherComponent* OnActor = OwnerActor->FindComponentByClass<USomaLipsyncMotionMatcherComponent>())
			{
				return OnActor;
			}
		}

		return nullptr;
	}
}

void USomaLipsyncAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();

	CachedMatcher = FindMatcherForAnimInstance(this);

	if (!CachedMatcher.IsValid())
	{
		UE_LOG(LogSomaLipsync, Verbose,
			TEXT("USomaLipsyncAnimInstance: no USomaLipsyncMotionMatcherComponent found on pawn or actor owner. Lipsync layer will stay at zero weight."));
	}
}

void USomaLipsyncAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

	if (!CachedMatcher.IsValid())
	{
		CachedMatcher = FindMatcherForAnimInstance(this);
	}

	if (USomaLipsyncMotionMatcherComponent* Matcher = CachedMatcher.Get())
	{
		const FLipsyncMatchResult Match = Matcher->GetCurrentMatch();
		SelectedLipsyncSequence = Match.SelectedSequence;
		SelectedLipsyncTime = Match.SelectedTime;
		SelectedMatchCost = Match.MatchCost;
		bHasValidLipsyncMatch = Match.bValid && (Match.SelectedSequence != nullptr);
		CurrentLipsyncViseme = Matcher->GetCurrentViseme();
	}
	else
	{
		bHasValidLipsyncMatch = false;
	}

	const float TargetWeight = bHasValidLipsyncMatch ? 1.f : 0.f;
	SmoothedLipsyncBlendWeight = FMath::FInterpTo(SmoothedLipsyncBlendWeight, TargetWeight, DeltaSeconds, BlendInterpSpeed);
}

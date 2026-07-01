#include "SomaGazeTargetComponent.h"

TArray<TWeakObjectPtr<USomaGazeTargetComponent>> USomaGazeTargetComponent::Registry;

USomaGazeTargetComponent::USomaGazeTargetComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

FVector USomaGazeTargetComponent::GetGazeWorldPosition() const
{
	if (const AActor* Owner = GetOwner())
	{
		return Owner->GetActorLocation();
	}
	return FVector::ZeroVector;
}

void USomaGazeTargetComponent::OnRegister()
{
	Super::OnRegister();
	Registry.AddUnique(this);
}

void USomaGazeTargetComponent::OnUnregister()
{
	Registry.RemoveAllSwap([this](const TWeakObjectPtr<USomaGazeTargetComponent>& Entry)
	{
		return !Entry.IsValid() || Entry.Get() == this;
	});
	Super::OnUnregister();
}

void USomaGazeTargetComponent::GetGazeTargetsForWorld(const UWorld* World, TArray<USomaGazeTargetComponent*>& OutComponents)
{
	OutComponents.Reset();
	if (!World)
	{
		return;
	}

	// Compact dead weak pointers while collecting matches for this world.
	for (int32 i = Registry.Num() - 1; i >= 0; --i)
	{
		USomaGazeTargetComponent* Component = Registry[i].Get();
		if (!Component)
		{
			Registry.RemoveAtSwap(i, 1, EAllowShrinking::No);
			continue;
		}

		if (Component->GetWorld() == World)
		{
			OutComponents.Add(Component);
		}
	}
}

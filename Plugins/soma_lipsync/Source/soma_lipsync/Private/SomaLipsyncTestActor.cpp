#include "SomaLipsyncTestActor.h"

#include "Components/InputComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"

#include "SomaLipsyncMatcher.h"
#include "SomaLipsyncModule.h"

ASomaLipsyncTestActor::ASomaLipsyncTestActor()
{
	PrimaryActorTick.bCanEverTick = false;
	AutoReceiveInput = EAutoReceiveInput::Player0;

	SkeletalMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("SkeletalMesh"));
	RootComponent = SkeletalMesh;

	Matcher = CreateDefaultSubobject<USomaLipsyncMotionMatcherComponent>(TEXT("Matcher"));
	// Test harness is for manual viseme cycling; voice subscription is off by default.
	Matcher->bAutoSubscribeToSomaVoice = false;
}

void ASomaLipsyncTestActor::BeginPlay()
{
	Super::BeginPlay();

	if (!bAutoBindInput)
	{
		return;
	}

	if (APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0))
	{
		EnableInput(PC);
	}

	if (!InputComponent)
	{
		UE_LOG(LogSomaLipsync, Warning,
			TEXT("ASomaLipsyncTestActor: no InputComponent after EnableInput; viseme keys not bound."));
		return;
	}

	auto BindVisemeKey = [this](FKey Key, ESomaLipsyncViseme Viseme)
	{
		FInputKeyBinding Binding(FInputChord(Key, /*bShift*/false, /*bCtrl*/false, /*bAlt*/false, /*bCmd*/false), IE_Pressed);
		Binding.KeyDelegate.GetDelegateForManualSet().BindLambda([this, Viseme]()
		{
			TriggerViseme(Viseme);
		});
		InputComponent->KeyBindings.Add(MoveTemp(Binding));
	};

	BindVisemeKey(EKeys::One,           ESomaLipsyncViseme::Sil);
	BindVisemeKey(EKeys::Two,           ESomaLipsyncViseme::PP);
	BindVisemeKey(EKeys::Three,         ESomaLipsyncViseme::FF);
	BindVisemeKey(EKeys::Four,          ESomaLipsyncViseme::TH);
	BindVisemeKey(EKeys::Five,          ESomaLipsyncViseme::DD);
	BindVisemeKey(EKeys::Six,           ESomaLipsyncViseme::KK);
	BindVisemeKey(EKeys::Seven,         ESomaLipsyncViseme::CH);
	BindVisemeKey(EKeys::Eight,         ESomaLipsyncViseme::SS);
	BindVisemeKey(EKeys::Nine,          ESomaLipsyncViseme::NN);
	BindVisemeKey(EKeys::Zero,          ESomaLipsyncViseme::RR);
	BindVisemeKey(EKeys::Hyphen,        ESomaLipsyncViseme::AA);
	BindVisemeKey(EKeys::Equals,        ESomaLipsyncViseme::EH);
	BindVisemeKey(EKeys::LeftBracket,   ESomaLipsyncViseme::IH);
	BindVisemeKey(EKeys::RightBracket,  ESomaLipsyncViseme::OH);
	BindVisemeKey(EKeys::Backslash,     ESomaLipsyncViseme::OU);

	UE_LOG(LogSomaLipsync, Display,
		TEXT("ASomaLipsyncTestActor: bound 15 viseme keys (1..0, -, =, [, ], \\) on '%s'."),
		*GetName());
}

void ASomaLipsyncTestActor::TriggerViseme(ESomaLipsyncViseme Viseme)
{
	if (Matcher)
	{
		Matcher->SetCurrentViseme(Viseme);
	}
}

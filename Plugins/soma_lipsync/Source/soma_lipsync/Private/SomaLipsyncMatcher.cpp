#include "SomaLipsyncMatcher.h"

#include "Animation/AnimSequence.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/IConsoleManager.h"

#include "LipsyncMotionDatabase.h"
#include "SomaLipsyncModule.h"
#include "SomaVoiceChatbot.h"
#include "VisemePoseTargetMap.h"

namespace
{
	static TAutoConsoleVariable<int32> CVarSomaLipsyncDebugDraw(
		TEXT("soma.lipsync.DebugDraw"),
		0,
		TEXT("Soma Lipsync on-screen debug overlay.\n")
		TEXT(" 0: off\n")
		TEXT(" 1: matcher state (viseme, clip, cost, switch counters)\n")
		TEXT(" 2: + per-bone cost breakdown\n")
		TEXT(" 3: + target pose translations\n"),
		ECVF_Cheat);

	const TCHAR* VisemeName(ESomaLipsyncViseme V)
	{
		switch (V)
		{
		case ESomaLipsyncViseme::Sil: return TEXT("Sil");
		case ESomaLipsyncViseme::PP:  return TEXT("PP");
		case ESomaLipsyncViseme::FF:  return TEXT("FF");
		case ESomaLipsyncViseme::TH:  return TEXT("TH");
		case ESomaLipsyncViseme::DD:  return TEXT("DD");
		case ESomaLipsyncViseme::KK:  return TEXT("KK");
		case ESomaLipsyncViseme::CH:  return TEXT("CH");
		case ESomaLipsyncViseme::SS:  return TEXT("SS");
		case ESomaLipsyncViseme::NN:  return TEXT("NN");
		case ESomaLipsyncViseme::RR:  return TEXT("RR");
		case ESomaLipsyncViseme::AA:  return TEXT("AA");
		case ESomaLipsyncViseme::EH:  return TEXT("EH");
		case ESomaLipsyncViseme::IH:  return TEXT("IH");
		case ESomaLipsyncViseme::OH:  return TEXT("OH");
		case ESomaLipsyncViseme::OU:  return TEXT("OU");
		default:                      return TEXT("?");
		}
	}
}

USomaLipsyncMotionMatcherComponent::USomaLipsyncMotionMatcherComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	LastPerBoneCost.Init(0.f, 4);
}

void USomaLipsyncMotionMatcherComponent::BeginPlay()
{
	Super::BeginPlay();

	if (bAutoSubscribeToSomaVoice)
	{
		TrySubscribeToVoiceChatbot();
	}

	bForceResearch = true;
	TimeSinceLastSearch = SearchInterval; // force a search on the first tick
	TimeSinceLastAccept = MinDwellSeconds; // first accept is allowed
}

void USomaLipsyncMotionMatcherComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnsubscribeFromVoiceChatbot();
	Super::EndPlay(EndPlayReason);
}

void USomaLipsyncMotionMatcherComponent::TrySubscribeToVoiceChatbot()
{
	UnsubscribeFromVoiceChatbot();

	ASomaVoiceChatbot* Target = nullptr;

	if (!VoiceChatbotActor.IsNull())
	{
		Target = VoiceChatbotActor.LoadSynchronous();
	}

	if (!Target)
	{
		if (UWorld* World = GetWorld())
		{
			TArray<AActor*> Found;
			UGameplayStatics::GetAllActorsOfClass(World, ASomaVoiceChatbot::StaticClass(), Found);
			if (Found.Num() > 0)
			{
				Target = Cast<ASomaVoiceChatbot>(Found[0]);
				if (Found.Num() > 1)
				{
					UE_LOG(LogSomaLipsync, Warning,
						TEXT("USomaLipsyncMotionMatcherComponent: multiple ASomaVoiceChatbot actors in world, binding to '%s'"),
						*Target->GetName());
				}
			}
		}
	}

	if (Target)
	{
		Target->OnVisemeFrame.AddDynamic(this, &USomaLipsyncMotionMatcherComponent::HandleVisemeFrame);
		BoundChatbot = Target;
		UE_LOG(LogSomaLipsync, Display,
			TEXT("USomaLipsyncMotionMatcherComponent on '%s' bound to ASomaVoiceChatbot '%s'"),
			*GetOwner()->GetName(), *Target->GetName());
	}
	else
	{
		UE_LOG(LogSomaLipsync, Verbose,
			TEXT("USomaLipsyncMotionMatcherComponent on '%s': no ASomaVoiceChatbot found yet (will tick + use SetCurrentViseme manually)."),
			*GetOwner()->GetName());
	}
}

void USomaLipsyncMotionMatcherComponent::UnsubscribeFromVoiceChatbot()
{
	if (BoundChatbot)
	{
		BoundChatbot->OnVisemeFrame.RemoveDynamic(this, &USomaLipsyncMotionMatcherComponent::HandleVisemeFrame);
		BoundChatbot = nullptr;
	}
}

void USomaLipsyncMotionMatcherComponent::HandleVisemeFrame(FSomaVisemeFrameData Frame)
{
	const TArray<float>& Scores = Frame.VisemeScores;
	if (Scores.Num() <= 0)
	{
		return;
	}

	const int32 ClampedNum = FMath::Min(Scores.Num(), (int32)ESomaLipsyncViseme::MAX);

	int32 BestIdx = 0;
	float BestScore = -FLT_MAX;
	for (int32 i = 0; i < ClampedNum; ++i)
	{
		if (Scores[i] > BestScore)
		{
			BestScore = Scores[i];
			BestIdx = i;
		}
	}

	if (BestScore < VisemeArgmaxThreshold)
	{
		return; // not enough signal -- keep current viseme (hysteresis)
	}

	const ESomaLipsyncViseme Candidate = static_cast<ESomaLipsyncViseme>(BestIdx);
	if (Candidate == CurrentViseme)
	{
		return;
	}

	const int32 CurrentIdx = static_cast<int32>(CurrentViseme);
	if (CurrentIdx >= 0 && CurrentIdx < ClampedNum)
	{
		const float CurrentScore = Scores[CurrentIdx];
		if (BestScore < CurrentScore + VisemeSwitchMargin)
		{
			return; // not enough margin to switch -- prevents flicker
		}
	}

	SetCurrentViseme(Candidate);
}

void USomaLipsyncMotionMatcherComponent::SetCurrentViseme(ESomaLipsyncViseme NewViseme)
{
	if (NewViseme == CurrentViseme)
	{
		return;
	}

	// Intentionally do NOT force a research here. The trajectory cost function
	// already factors the upcoming viseme into the target window, so we rely on
	// SearchInterval to schedule the next search. Forcing a research on every
	// argmax flicker was the dominant source of per-phoneme clip popping.
	CurrentViseme = NewViseme;
}

void USomaLipsyncMotionMatcherComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bAutoSubscribeToSomaVoice && !BoundChatbot)
	{
		// Voice actor may spawn after this component begins play; retry occasionally.
		static float RetryAccum = 0.f;
		RetryAccum += DeltaTime;
		if (RetryAccum > 1.f)
		{
			RetryAccum = 0.f;
			TrySubscribeToVoiceChatbot();
		}
	}

	BuildTargetWindow(CurrentTargetWindow, CurrentTargetPose);

	// Advance the currently selected snippet so it doesn't freeze between searches (PDF 8.1).
	if (CurrentMatch.bValid && CurrentMatch.SelectedSequence)
	{
		const float PlayLength = CurrentMatch.SelectedSequence->GetPlayLength();
		if (PlayLength > 0.f)
		{
			float Advanced = CurrentMatch.SelectedTime + DeltaTime;
			if (Advanced > PlayLength)
			{
				Advanced = FMath::Fmod(Advanced, PlayLength);
			}
			CurrentMatch.SelectedTime = Advanced;
		}
	}

	TimeSinceLastSearch += DeltaTime;
	TimeSinceLastAccept += DeltaTime;

	const bool bDueForSearch = (TimeSinceLastSearch >= SearchInterval);
	const bool bMatchInvalid = !CurrentMatch.bValid || !CurrentMatch.SelectedSequence;

	if (bForceResearch || bMatchInvalid || bDueForSearch)
	{
		++SearchesThisWindow;
		TimeSinceLastSearch = 0.f;

		// Guard the cached index against Database swaps / rebuilds that
		// invalidate the old sample identity. Either condition means
		// "treat the current selection as gone" -- the next search must
		// freely accept the new best candidate.
		if (CurrentMatchSampleIndex != INDEX_NONE && Database)
		{
			const bool bIdxOutOfRange = (CurrentMatchSampleIndex >= Database->Samples.Num());
			const bool bSequenceMoved = !bIdxOutOfRange &&
				Database->Samples[CurrentMatchSampleIndex].Sequence != CurrentMatch.SelectedSequence;
			if (bIdxOutOfRange || bSequenceMoved)
			{
				CurrentMatchSampleIndex = INDEX_NONE;
			}
		}

		FLipsyncMatchResult Candidate;
		TArray<float> CandidatePerBone;
		bool bCandidateUsedWindow = false;
		int32 CandidateSampleIndex = INDEX_NONE;
		float CurrentSampleLiveCost = TNumericLimits<float>::Max();
		const bool bFoundCandidate = SearchBestMatch(CurrentTargetWindow, CurrentTargetPose, Candidate,
			&CandidatePerBone, &bCandidateUsedWindow, &CandidateSampleIndex,
			CurrentMatchSampleIndex, &CurrentSampleLiveCost);

		if (bFoundCandidate)
		{
			// Re-cost the currently-selected sample against TODAY's target
			// window. The old CurrentMatch.MatchCost was a global min against
			// an older target and is meaningless as an accept baseline (it
			// was the dominant source of the "matcher locks on first clip"
			// bug). Fall back to the stored cost only if the live re-cost is
			// unavailable (e.g. first ever search, or database swap mid-tick).
			const bool bHaveLiveBaseline = (CurrentSampleLiveCost < TNumericLimits<float>::Max());
			const float CurrentBaselineCost = bHaveLiveBaseline ? CurrentSampleLiveCost : CurrentMatch.MatchCost;

			const bool bDwellOver = (TimeSinceLastAccept >= MinDwellSeconds);
			const bool bAccept =
				bForceResearch ||
				bMatchInvalid ||
				(bDwellOver && Candidate.MatchCost < CurrentBaselineCost - SwitchThreshold);

			if (bAccept)
			{
				CurrentMatch = Candidate;
				CurrentMatchSampleIndex = CandidateSampleIndex;
				LastPerBoneCost = CandidatePerBone;
				bLastMatchUsedWindow = bCandidateUsedWindow;
				TimeSinceLastAccept = 0.f;
				++SwitchesThisWindow;
			}
			else
			{
				// Keep MatchCost honest for the overlay -- this is the cost
				// of the current sample against the CURRENT target, not the
				// stale cost from when it was first accepted.
				if (bHaveLiveBaseline)
				{
					CurrentMatch.MatchCost = CurrentSampleLiveCost;
				}
				if (CandidatePerBone.Num() == 4)
				{
					LastPerBoneCost = CandidatePerBone;
				}
			}
		}

		bForceResearch = false;
	}

	// Switch/search rate readout.
	DebugWindowAccum += DeltaTime;
	if (DebugWindowAccum >= 1.f)
	{
		SearchesPerSecondReadout = FMath::RoundToInt(SearchesThisWindow / DebugWindowAccum);
		SwitchesPerSecondReadout = FMath::RoundToInt(SwitchesThisWindow / DebugWindowAccum);
		SearchesThisWindow = 0;
		SwitchesThisWindow = 0;
		DebugWindowAccum = 0.f;
	}

	UpdateDebugOverlay();
}

namespace
{
	FTransform LerpTransform(const FTransform& A, const FTransform& B, float Alpha)
	{
		FTransform Out;
		Out.Blend(A, B, Alpha);
		return Out;
	}

	FLipsyncMouthPose LerpMouthPose(const FLipsyncMouthPose& A, const FLipsyncMouthPose& B, float Alpha)
	{
		FLipsyncMouthPose Out;
		Out.LowLip      = LerpTransform(A.LowLip,      B.LowLip,      Alpha);
		Out.UpLip       = LerpTransform(A.UpLip,       B.UpLip,       Alpha);
		Out.LeftCorner  = LerpTransform(A.LeftCorner,  B.LeftCorner,  Alpha);
		Out.RightCorner = LerpTransform(A.RightCorner, B.RightCorner, Alpha);
		return Out;
	}
}

bool USomaLipsyncMotionMatcherComponent::BuildTargetWindow(FLipsyncPoseWindow& OutWindow, FLipsyncMouthPose& OutNowPose) const
{
	OutWindow.Stations.SetNum(SomaLipsyncTraj::NumStations);
	OutWindow.bValid = false;
	OutNowPose = FLipsyncMouthPose();

	if (!PoseTargetMap)
	{
		return false;
	}

	// Hard target for the current argmax viseme. Used as the all-stations
	// fallback when no utterance is in progress and as a safety net inside the
	// per-station soft interp.
	FLipsyncMouthPose FallbackPose;
	const bool bHaveFallback = PoseTargetMap->GetPose(CurrentViseme, FallbackPose);

	const UWorld* World = GetWorld();
	const float WorldTime = World ? World->GetTimeSeconds() : 0.f;

	bool bAnyStationFromTimeline = false;
	for (int32 k = 0; k < SomaLipsyncTraj::NumStations; ++k)
	{
		const float StationWorldTime = WorldTime + SomaLipsyncTraj::StationOffsetsSeconds[k];

		bool bStationFilled = false;
		if (BoundChatbot)
		{
			int32 PrevViseme = 0;
			int32 NextViseme = 0;
			float PrevMid = 0.f;
			float NextMid = 0.f;
			if (BoundChatbot->GetVisemeAtWorldTime(StationWorldTime, PrevViseme, PrevMid, NextViseme, NextMid))
			{
				bAnyStationFromTimeline = true;
				const ESomaLipsyncViseme PrevV = static_cast<ESomaLipsyncViseme>(
					FMath::Clamp(PrevViseme, 0, (int32)ESomaLipsyncViseme::MAX - 1));
				const ESomaLipsyncViseme NextV = static_cast<ESomaLipsyncViseme>(
					FMath::Clamp(NextViseme, 0, (int32)ESomaLipsyncViseme::MAX - 1));

				FLipsyncMouthPose PrevPose;
				FLipsyncMouthPose NextPose;
				const bool bHavePrev = PoseTargetMap->GetPose(PrevV, PrevPose);
				const bool bHaveNext = PoseTargetMap->GetPose(NextV, NextPose);
				if (bHavePrev && bHaveNext)
				{
					const float Span = FMath::Max(NextMid - PrevMid, KINDA_SMALL_NUMBER);
					const float Alpha = FMath::Clamp((StationWorldTime - PrevMid) / Span, 0.f, 1.f);
					OutWindow.Stations[k] = LerpMouthPose(PrevPose, NextPose, Alpha);
					bStationFilled = true;
				}
			}
		}

		if (!bStationFilled)
		{
			OutWindow.Stations[k] = FallbackPose;
		}
	}

	OutWindow.bValid = bHaveFallback || (BoundChatbot != nullptr);
	bLastTimelineActive = bAnyStationFromTimeline;

	// Back-fill the single-pose field used by the debug overlay and the
	// legacy/un-rebuilt-database fallback path from the 'now' station (index 1).
	OutNowPose = OutWindow.Stations[1];
	return OutWindow.bValid;
}

bool USomaLipsyncMotionMatcherComponent::SearchBestMatch(const FLipsyncPoseWindow& InTargetWindow,
	const FLipsyncMouthPose& InNowPose,
	FLipsyncMatchResult& OutResult,
	TArray<float>* OutPerBoneCost,
	bool* OutBestUsedWindow,
	int32* OutBestSampleIndex,
	int32 InCurrentSampleIndex,
	float* OutCurrentSampleCost) const
{
	OutResult = FLipsyncMatchResult();
	if (OutBestUsedWindow)
	{
		*OutBestUsedWindow = false;
	}
	if (OutBestSampleIndex)
	{
		*OutBestSampleIndex = INDEX_NONE;
	}
	if (OutCurrentSampleCost)
	{
		*OutCurrentSampleCost = TNumericLimits<float>::Max();
	}

	if (!Database || Database->Samples.Num() == 0)
	{
		return false;
	}

	float BestCost = TNumericLimits<float>::Max();
	int32 BestIdx = INDEX_NONE;
	TArray<float> BestPerBone;
	bool bBestUsedWindow = false;

	const int32 NumSamples = Database->Samples.Num();
	for (int32 i = 0; i < NumSamples; ++i)
	{
		const FLipsyncPoseSample& Sample = Database->Samples[i];
		if (!Sample.Sequence)
		{
			continue;
		}

		TArray<float> PerBone;
		float Cost;
		bool bUsedWindow = false;
		if (Sample.Window.bValid && Sample.Window.Stations.Num() == SomaLipsyncTraj::NumStations
			&& InTargetWindow.Stations.Num() == SomaLipsyncTraj::NumStations)
		{
			Cost = ComputeWindowCost(InTargetWindow, Sample.Window, &PerBone);
			bUsedWindow = true;
		}
		else
		{
			// Fallback for legacy / un-rebuilt databases: single-pose cost
			// against the 'now' station target.
			Cost = ComputeCost(InNowPose, Sample.MouthPose, &PerBone);
		}

		if (OutCurrentSampleCost && i == InCurrentSampleIndex)
		{
			*OutCurrentSampleCost = Cost;
		}

		if (Cost < BestCost)
		{
			BestCost = Cost;
			BestIdx = i;
			BestPerBone = MoveTemp(PerBone);
			bBestUsedWindow = bUsedWindow;
		}
	}

	if (BestIdx == INDEX_NONE)
	{
		return false;
	}

	const FLipsyncPoseSample& Best = Database->Samples[BestIdx];
	OutResult.SelectedSequence = Best.Sequence;
	OutResult.SelectedTime = Best.Time;
	OutResult.SelectedLabel = Best.SourceLabel;
	OutResult.MatchCost = BestCost;
	OutResult.bValid = true;

	if (OutPerBoneCost)
	{
		*OutPerBoneCost = MoveTemp(BestPerBone);
	}
	if (OutBestUsedWindow)
	{
		*OutBestUsedWindow = bBestUsedWindow;
	}
	if (OutBestSampleIndex)
	{
		*OutBestSampleIndex = BestIdx;
	}
	return true;
}

void USomaLipsyncMotionMatcherComponent::EnsureDatabaseStats() const
{
	if (CachedStatsDatabase.Get() == Database)
	{
		return;
	}

	CachedStatsDatabase = Database;
	CachedWindowValidCount = 0;
	CachedWindowInvalidCount = 0;
	CachedSampleTotal = 0;

	if (!Database)
	{
		return;
	}

	for (const FLipsyncPoseSample& Sample : Database->Samples)
	{
		if (!Sample.Sequence)
		{
			continue;
		}
		++CachedSampleTotal;
		if (Sample.Window.bValid && Sample.Window.Stations.Num() == SomaLipsyncTraj::NumStations)
		{
			++CachedWindowValidCount;
		}
		else
		{
			++CachedWindowInvalidCount;
		}
	}
}

float USomaLipsyncMotionMatcherComponent::ComputeCost(const FLipsyncMouthPose& A, const FLipsyncMouthPose& B, TArray<float>* OutPerBoneCost) const
{
	auto BoneCost = [this](const FTransform& Ta, const FTransform& Tb, float PosWeight) -> float
	{
		float Cost = PosWeight * static_cast<float>(FVector::DistSquared(Ta.GetLocation(), Tb.GetLocation()));
		if (RotationWeight > 0.f)
		{
			const float RotErr = static_cast<float>(Ta.GetRotation().AngularDistance(Tb.GetRotation()));
			Cost += RotationWeight * RotErr * RotErr;
		}
		return Cost;
	};

	const float CostLow    = BoneCost(A.LowLip,      B.LowLip,      LowLipWeight);
	const float CostUp     = BoneCost(A.UpLip,       B.UpLip,       UpLipWeight);
	const float CostLeft   = BoneCost(A.LeftCorner,  B.LeftCorner,  CornerWeight);
	const float CostRight  = BoneCost(A.RightCorner, B.RightCorner, CornerWeight);

	if (OutPerBoneCost)
	{
		OutPerBoneCost->Reset(4);
		OutPerBoneCost->Add(CostLow);
		OutPerBoneCost->Add(CostUp);
		OutPerBoneCost->Add(CostLeft);
		OutPerBoneCost->Add(CostRight);
	}

	return CostLow + CostUp + CostLeft + CostRight;
}

float USomaLipsyncMotionMatcherComponent::ComputeWindowCost(const FLipsyncPoseWindow& Target,
	const FLipsyncPoseWindow& Candidate,
	TArray<float>* OutPerBoneCost) const
{
	const float StationWeights[SomaLipsyncTraj::NumStations] =
	{
		PastWeight,
		NowWeight,
		Future1Weight,
		Future2Weight,
		Future3Weight,
	};

	float Accum[4] = { 0.f, 0.f, 0.f, 0.f };
	float Total = 0.f;
	float WeightSum = 0.f;

	const int32 NumStations = FMath::Min3(
		Target.Stations.Num(),
		Candidate.Stations.Num(),
		(int32)SomaLipsyncTraj::NumStations);

	for (int32 k = 0; k < NumStations; ++k)
	{
		const float W = StationWeights[k];
		if (W <= 0.f)
		{
			continue;
		}

		WeightSum += W;

		TArray<float> PerBone;
		const float StationCost = ComputeCost(Target.Stations[k], Candidate.Stations[k], &PerBone);
		Total += W * StationCost;
		if (PerBone.Num() == 4)
		{
			Accum[0] += W * PerBone[0];
			Accum[1] += W * PerBone[1];
			Accum[2] += W * PerBone[2];
			Accum[3] += W * PerBone[3];
		}
	}

	// Normalize by total station weight so window-path costs are directly
	// comparable to single-pose fallback costs. Without this, a window-path
	// sample pays a structural ~Sum(StationWeights) penalty (~3.3x with
	// defaults) vs an equivalent-quality fallback sample, and the matcher
	// systematically prefers samples whose Window.bValid is false -- visible
	// in the overlay as 'Cost path: Fallback(single-pose)' even when the
	// trajectory bake reports a high % of valid windows.
	const float Inv = (WeightSum > KINDA_SMALL_NUMBER) ? (1.f / WeightSum) : 1.f;
	Total *= Inv;
	Accum[0] *= Inv;
	Accum[1] *= Inv;
	Accum[2] *= Inv;
	Accum[3] *= Inv;

	if (OutPerBoneCost)
	{
		OutPerBoneCost->Reset(4);
		OutPerBoneCost->Add(Accum[0]);
		OutPerBoneCost->Add(Accum[1]);
		OutPerBoneCost->Add(Accum[2]);
		OutPerBoneCost->Add(Accum[3]);
	}

	return Total;
}

void USomaLipsyncMotionMatcherComponent::UpdateDebugOverlay() const
{
	const int32 Mode = CVarSomaLipsyncDebugDraw.GetValueOnGameThread();
	if (Mode <= 0 || !GEngine)
	{
		return;
	}

	EnsureDatabaseStats();

	const uint64 BaseKey = reinterpret_cast<uint64>(this) + 0x10aaa1;
	const FColor HeaderColor = FColor::Cyan;
	const FColor LineColor   = FColor::White;
	const FColor WarnColor   = FColor::Yellow;
	const FColor OkColor     = FColor::Green;

	const FString OwnerName = GetOwner() ? GetOwner()->GetName() : TEXT("<no owner>");
	GEngine->AddOnScreenDebugMessage((int32)(BaseKey + 0), 0.f, HeaderColor,
		FString::Printf(TEXT("[SomaLipsync] %s"), *OwnerName));

	GEngine->AddOnScreenDebugMessage((int32)(BaseKey + 1), 0.f, LineColor,
		FString::Printf(TEXT("  Viseme: %s   Searches/s: %d   Switches/s: %d"),
			VisemeName(CurrentViseme), SearchesPerSecondReadout, SwitchesPerSecondReadout));

	if (CurrentMatch.bValid && CurrentMatch.SelectedSequence)
	{
		GEngine->AddOnScreenDebugMessage((int32)(BaseKey + 2), 0.f, LineColor,
			FString::Printf(TEXT("  Clip: %s   t=%.3fs   Cost=%.5f"),
				*CurrentMatch.SelectedLabel, CurrentMatch.SelectedTime, CurrentMatch.MatchCost));
	}
	else
	{
		GEngine->AddOnScreenDebugMessage((int32)(BaseKey + 2), 0.f, WarnColor,
			TEXT("  No valid match (Database/PoseTargetMap missing or empty)"));
	}

	// Trajectory observability: prove the bake ran + which cost path is live.
	{
		const float ValidPct = (CachedSampleTotal > 0)
			? (100.f * static_cast<float>(CachedWindowValidCount) / static_cast<float>(CachedSampleTotal))
			: 0.f;
		const TCHAR* PathStr = bLastMatchUsedWindow ? TEXT("Window") : TEXT("Fallback(single-pose)");
		const TCHAR* TimelineStr = bLastTimelineActive ? TEXT("Active") : TEXT("Idle");
		const FColor TrajColor = (CachedWindowValidCount > 0 && bLastMatchUsedWindow) ? OkColor : WarnColor;
		GEngine->AddOnScreenDebugMessage((int32)(BaseKey + 5), 0.f, TrajColor,
			FString::Printf(TEXT("  Trajectory: %d/%d windows valid (%.0f%%)   Cost path: %s   Timeline: %s"),
				CachedWindowValidCount, CachedSampleTotal, ValidPct, PathStr, TimelineStr));
	}

	// PoseTargetMap observability: the most common cause of "matcher locked on
	// a 0-cost identity sample" is that the user never clicked
	// Capture From Source Anims on DA_VisemePoseTargets (or SourceVisemeAnims
	// is empty), so every viseme target degenerates to identity.
	{
		const int32 NumMapEntries = PoseTargetMap ? PoseTargetMap->Map.Num() : 0;
		const int32 ExpectedEntries = static_cast<int32>(ESomaLipsyncViseme::MAX);
		const bool bMapOk = (PoseTargetMap != nullptr) && (NumMapEntries >= ExpectedEntries);
		const FColor MapColor = bMapOk ? OkColor : WarnColor;
		const TCHAR* MapNote = (PoseTargetMap == nullptr)
			? TEXT(" -- assign DA_VisemePoseTargets")
			: ((NumMapEntries == 0)
				? TEXT(" -- click Capture From Source Anims on DA_VisemePoseTargets")
				: ((NumMapEntries < ExpectedEntries) ? TEXT(" -- some visemes missing") : TEXT("")));
		GEngine->AddOnScreenDebugMessage((int32)(BaseKey + 6), 0.f, MapColor,
			FString::Printf(TEXT("  PoseMap: %d/%d entries%s"),
				NumMapEntries, ExpectedEntries, MapNote));
	}

	if (Mode >= 2 && LastPerBoneCost.Num() == 4)
	{
		GEngine->AddOnScreenDebugMessage((int32)(BaseKey + 3), 0.f, LineColor,
			FString::Printf(TEXT("  Per-bone cost: LowLip=%.5f UpLip=%.5f LCorner=%.5f RCorner=%.5f"),
				LastPerBoneCost[0], LastPerBoneCost[1], LastPerBoneCost[2], LastPerBoneCost[3]));
	}

	if (Mode >= 3)
	{
		const FVector LowT = CurrentTargetPose.LowLip.GetLocation();
		const FVector UpT  = CurrentTargetPose.UpLip.GetLocation();
		const FVector LcT  = CurrentTargetPose.LeftCorner.GetLocation();
		const FVector RcT  = CurrentTargetPose.RightCorner.GetLocation();
		GEngine->AddOnScreenDebugMessage((int32)(BaseKey + 4), 0.f, LineColor,
			FString::Printf(TEXT("  Target T: Low(%.2f,%.2f,%.2f) Up(%.2f,%.2f,%.2f) Lc(%.2f,%.2f,%.2f) Rc(%.2f,%.2f,%.2f)"),
				LowT.X, LowT.Y, LowT.Z, UpT.X, UpT.Y, UpT.Z,
				LcT.X, LcT.Y, LcT.Z, RcT.X, RcT.Y, RcT.Z));
	}
}

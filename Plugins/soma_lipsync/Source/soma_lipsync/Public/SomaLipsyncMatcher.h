#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SomaLipsyncTypes.h"
#include "SomaVoiceChatbot.h" // for FSomaVisemeFrameData
#include "SomaLipsyncMatcher.generated.h"

class UVisemePoseTargetMap;
class ULipsyncMotionDatabase;
class ASomaVoiceChatbot;

/**
 * Custom motion-matching component for lip sync (PDF "Option A").
 * - Subscribes to ASomaVoiceChatbot::OnVisemeFrame.
 * - Picks a viseme via argmax + hysteresis (or via SetCurrentViseme manually).
 * - Brute-force searches ULipsyncMotionDatabase against the viseme's target pose.
 * - Exposes the resulting clip + time to AnimBP via GetCurrentMatch.
 */
UCLASS(ClassGroup = (SomaLipsync), meta = (BlueprintSpawnableComponent), DisplayName = "Soma Lipsync Motion Matcher")
class SOMA_LIPSYNC_API USomaLipsyncMotionMatcherComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USomaLipsyncMotionMatcherComponent();

	// ---- Configuration ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Data")
	TObjectPtr<ULipsyncMotionDatabase> Database;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Data")
	TObjectPtr<UVisemePoseTargetMap> PoseTargetMap;

	/** Run a search at most every SearchInterval seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Search", meta = (ClampMin = "0.0"))
	float SearchInterval = 0.05f;

	/** Hysteresis: only switch sample when Candidate.Cost < Current.Cost - SwitchThreshold. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Search", meta = (ClampMin = "0.0"))
	float SwitchThreshold = 0.001f;

	/**
	 * Minimum time the matcher must keep a freshly-accepted clip before it is
	 * eligible to be replaced. Acts together with SwitchThreshold to suppress
	 * per-phoneme popping; the cost-window matcher does most of the work, this
	 * is just a safety net.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Search", meta = (ClampMin = "0.0"))
	float MinDwellSeconds = 0.15f;

	/** Per-bone position cost weights. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Cost", meta = (ClampMin = "0.0"))
	float LowLipWeight = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Cost", meta = (ClampMin = "0.0"))
	float UpLipWeight = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Cost", meta = (ClampMin = "0.0"))
	float CornerWeight = 1.0f;

	/** Rotation cost is plumbed but disabled by default (PDF v1 = position-only). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Cost", meta = (ClampMin = "0.0"))
	float RotationWeight = 0.f;

	/**
	 * Per-station weights for the trajectory cost function. Stations are laid
	 * out per SomaLipsyncTraj::StationOffsetsSeconds: [past, now, f1, f2, f3].
	 * Setting all future weights to zero collapses the matcher to current-pose-
	 * only behaviour (useful for A/B testing).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Trajectory", meta = (ClampMin = "0.0"))
	float PastWeight = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Trajectory", meta = (ClampMin = "0.0"))
	float NowWeight = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Trajectory", meta = (ClampMin = "0.0"))
	float Future1Weight = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Trajectory", meta = (ClampMin = "0.0"))
	float Future2Weight = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Trajectory", meta = (ClampMin = "0.0"))
	float Future3Weight = 0.4f;

	// ---- Voice integration ----

	/** Auto-find an ASomaVoiceChatbot in the world if VoiceChatbotActor is null. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Voice")
	bool bAutoSubscribeToSomaVoice = true;

	/** Optional explicit voice actor reference (level-placed). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Voice")
	TSoftObjectPtr<ASomaVoiceChatbot> VoiceChatbotActor;

	/** Argmax score must exceed this for the corresponding viseme to win. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Voice", meta = (ClampMin = "0.0"))
	float VisemeArgmaxThreshold = 0.05f;

	/** Hysteresis on viseme switching: a new viseme must beat the current one by this margin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Voice", meta = (ClampMin = "0.0"))
	float VisemeSwitchMargin = 0.05f;

	/** EXPERIMENTAL: weighted-blended target pose. Body is stubbed; keep false for v1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lipsync|Experimental")
	bool bUseWeightedTargetBlend = false;

	// ---- Public API ----

	UFUNCTION(BlueprintCallable, Category = "Lipsync")
	void SetCurrentViseme(ESomaLipsyncViseme NewViseme);

	UFUNCTION(BlueprintPure, Category = "Lipsync")
	ESomaLipsyncViseme GetCurrentViseme() const { return CurrentViseme; }

	UFUNCTION(BlueprintPure, Category = "Lipsync")
	FLipsyncMatchResult GetCurrentMatch() const { return CurrentMatch; }

	UFUNCTION(BlueprintPure, Category = "Lipsync")
	FLipsyncMouthPose GetCurrentTargetPose() const { return CurrentTargetPose; }

	// ---- Debug accessors used by the on-screen overlay ----

	int32 GetSearchesPerSecond() const { return SearchesPerSecondReadout; }
	int32 GetSwitchesPerSecond() const { return SwitchesPerSecondReadout; }
	const TArray<float>& GetLastPerBoneCost() const { return LastPerBoneCost; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION()
	void HandleVisemeFrame(FSomaVisemeFrameData Frame);

private:
	void TrySubscribeToVoiceChatbot();
	void UnsubscribeFromVoiceChatbot();

	/**
	 * Build the trajectory target window (5 mouth poses around the current
	 * world time) by querying the bound chatbot for the upcoming visemes and
	 * lerping the viseme target poses by midpoint. When no utterance is in
	 * progress, all stations fall back to the current viseme's hard target.
	 * Also back-fills OutNowPose from the 'now' station for the debug overlay
	 * and the single-pose fallback search path.
	 */
	bool BuildTargetWindow(FLipsyncPoseWindow& OutWindow, FLipsyncMouthPose& OutNowPose) const;

	/**
	 * Run a brute-force search of Database->Samples against InTargetWindow.
	 * Samples whose Window.bValid is false fall back to the single-pose cost
	 * against InNowPose so legacy / un-rebuilt databases still work.
	 *
	 * OutBestUsedWindow reports which cost path the winning candidate used
	 * (true=trajectory window, false=single-pose fallback) so the debug overlay
	 * can prove trajectory matching is actually engaged.
	 */
	bool SearchBestMatch(const FLipsyncPoseWindow& InTargetWindow,
		const FLipsyncMouthPose& InNowPose,
		FLipsyncMatchResult& OutResult,
		TArray<float>* OutPerBoneCost = nullptr,
		bool* OutBestUsedWindow = nullptr,
		int32* OutBestSampleIndex = nullptr,
		int32 InCurrentSampleIndex = INDEX_NONE,
		float* OutCurrentSampleCost = nullptr) const;

	/** Recompute CachedWindow*Count for the current Database pointer (cheap; only does work when Database changes). */
	void EnsureDatabaseStats() const;

	/** Cost between two mouth poses (position-only by default; +rotation if RotationWeight>0). */
	float ComputeCost(const FLipsyncMouthPose& A, const FLipsyncMouthPose& B, TArray<float>* OutPerBoneCost = nullptr) const;

	/** Weighted sum of per-station ComputeCost across all stations in the window. */
	float ComputeWindowCost(const FLipsyncPoseWindow& Target, const FLipsyncPoseWindow& Candidate, TArray<float>* OutPerBoneCost = nullptr) const;

	void UpdateDebugOverlay() const;

	UPROPERTY(Transient)
	TObjectPtr<ASomaVoiceChatbot> BoundChatbot = nullptr;

	UPROPERTY(Transient)
	ESomaLipsyncViseme CurrentViseme = ESomaLipsyncViseme::Sil;

	UPROPERTY(Transient)
	FLipsyncMouthPose CurrentTargetPose;

	UPROPERTY(Transient)
	FLipsyncPoseWindow CurrentTargetWindow;

	UPROPERTY(Transient)
	FLipsyncMatchResult CurrentMatch;

	float TimeSinceLastSearch = 0.f;
	float TimeSinceLastAccept = 0.f;
	bool bForceResearch = true;

	// Index of the currently-selected sample inside Database->Samples, so we
	// can re-cost it against today's target window each search. Without this
	// we'd compare a stale "cost vs. original target" against the new
	// candidate's "cost vs. current target" and the matcher would latch onto
	// the first accepted clip forever.
	int32 CurrentMatchSampleIndex = INDEX_NONE;

	int32 SearchesThisWindow = 0;
	int32 SwitchesThisWindow = 0;
	float DebugWindowAccum = 0.f;
	int32 SearchesPerSecondReadout = 0;
	int32 SwitchesPerSecondReadout = 0;

	TArray<float> LastPerBoneCost;

	// ---- Debug observability ----
	// Cached per-Database stats so the overlay can prove the bake ran with the
	// new trajectory data (Window.bValid populated). Recomputed lazily whenever
	// the Database pointer changes.
	mutable TWeakObjectPtr<const ULipsyncMotionDatabase> CachedStatsDatabase;
	mutable int32 CachedWindowValidCount = 0;
	mutable int32 CachedWindowInvalidCount = 0;
	mutable int32 CachedSampleTotal = 0;

	// True iff the winning candidate from the last accepted search used the
	// trajectory window cost path (false = single-pose fallback path).
	bool bLastMatchUsedWindow = false;

	// True iff the bound chatbot reported an active viseme timeline entry the
	// last time BuildTargetWindow ran.
	mutable bool bLastTimelineActive = false;
};

#include "VisemePoseTargetMap.h"

#include "SomaLipsyncModule.h"

bool UVisemePoseTargetMap::GetPose(ESomaLipsyncViseme Viseme, FLipsyncMouthPose& OutPose) const
{
	if (const FLipsyncMouthPose* Found = Map.Find(Viseme))
	{
		OutPose = *Found;
		return true;
	}

	if (const FLipsyncMouthPose* Sil = Map.Find(ESomaLipsyncViseme::Sil))
	{
		OutPose = *Sil;
		return true;
	}

	OutPose = FLipsyncMouthPose();
	return false;
}

#if WITH_EDITOR
void UVisemePoseTargetMap::CaptureFromSourceAnims()
{
	const FSomaLipsyncEditorBridge& Bridge = FSomaLipsyncEditorBridge::Get();
	if (Bridge.CaptureViseme)
	{
		Bridge.CaptureViseme(this);
	}
	else
	{
		UE_LOG(LogSomaLipsync, Warning,
			TEXT("UVisemePoseTargetMap::CaptureFromSourceAnims: editor bridge not installed. Make sure soma_lipsyncEditor is loaded."));
	}
}
#endif

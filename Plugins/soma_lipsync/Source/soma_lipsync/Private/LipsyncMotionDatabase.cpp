#include "LipsyncMotionDatabase.h"

#include "SomaLipsyncModule.h"

ULipsyncMotionDatabase::ULipsyncMotionDatabase()
{
	RequiredBones = {
		SomaLipsyncBones::LowLip,
		SomaLipsyncBones::UpLip,
		SomaLipsyncBones::LCorner,
		SomaLipsyncBones::RCorner
	};
}

void ULipsyncMotionDatabase::PostLoad()
{
	Super::PostLoad();
	RefreshOutputSummary();
}

void ULipsyncMotionDatabase::RefreshOutputSummary()
{
	NumSamples = Samples.Num();

	int32 ValidWindows = 0;
	for (const FLipsyncPoseSample& Sample : Samples)
	{
		if (Sample.Window.bValid && Sample.Window.Stations.Num() == SomaLipsyncTraj::NumStations)
		{
			++ValidWindows;
		}
	}
	NumValidTrajectoryWindows = ValidWindows;
}

#if WITH_EDITOR
void ULipsyncMotionDatabase::BuildIndex()
{
	const FSomaLipsyncEditorBridge& Bridge = FSomaLipsyncEditorBridge::Get();
	if (Bridge.BuildDatabaseIndex)
	{
		Bridge.BuildDatabaseIndex(this);
	}
	else
	{
		UE_LOG(LogSomaLipsync, Warning,
			TEXT("ULipsyncMotionDatabase::BuildIndex: editor bridge not installed. Make sure soma_lipsyncEditor is loaded."));
	}

	RefreshOutputSummary();
}
#endif

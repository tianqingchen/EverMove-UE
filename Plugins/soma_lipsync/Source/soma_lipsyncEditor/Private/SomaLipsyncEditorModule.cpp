#include "SomaLipsyncEditorModule.h"

#include "Modules/ModuleManager.h"
#include "Animation/AnimSequence.h"

#include "SomaLipsyncModule.h"
#include "SomaLipsyncBoneExtractor.h"
#include "VisemePoseTargetMap.h"
#include "LipsyncMotionDatabase.h"

DEFINE_LOG_CATEGORY(LogSomaLipsyncEditor);

namespace
{
	void CaptureFromSourceAnims(UVisemePoseTargetMap* Asset)
	{
		if (!Asset)
		{
			return;
		}

		Asset->Map.Reset();
		int32 Captured = 0;

		for (const TPair<ESomaLipsyncViseme, TObjectPtr<UAnimSequence>>& Pair : Asset->SourceVisemeAnims)
		{
			if (!Pair.Value)
			{
				continue;
			}

			float SampleTime = Asset->CaptureTime;
			if (const float* Override = Asset->CaptureTimeOverrides.Find(Pair.Key))
			{
				SampleTime = *Override;
			}

			FLipsyncMouthPose Pose;
			if (FSomaLipsyncBoneExtractor::ExtractMouthPose(Pair.Value, SampleTime, Pose))
			{
				Asset->Map.Add(Pair.Key, Pose);
				++Captured;
			}
			else
			{
				UE_LOG(LogSomaLipsyncEditor, Warning,
					TEXT("CaptureFromSourceAnims: failed to extract pose for viseme %d from '%s'"),
					(int32)Pair.Key, *Pair.Value->GetName());
			}
		}

		Asset->MarkPackageDirty();

		UE_LOG(LogSomaLipsyncEditor, Display,
			TEXT("CaptureFromSourceAnims: captured %d/%d viseme target poses on '%s'"),
			Captured, Asset->SourceVisemeAnims.Num(), *Asset->GetName());
	}

	void BuildDatabaseIndex(ULipsyncMotionDatabase* Asset)
	{
		if (!Asset)
		{
			return;
		}

		Asset->Samples.Reset();

		const float SampleRate = FMath::Max(1.f, Asset->SampleRate);
		const float DeltaT = 1.f / SampleRate;

		TArray<FName> Bones = Asset->RequiredBones;
		if (Bones.Num() == 0)
		{
			Bones = {
				SomaLipsyncBones::LowLip,
				SomaLipsyncBones::UpLip,
				SomaLipsyncBones::LCorner,
				SomaLipsyncBones::RCorner
			};
		}

		int32 SequenceCount = 0;
		int32 WindowsValid = 0;
		int32 WindowsInvalid = 0;
		for (const TObjectPtr<UAnimSequence>& Seq : Asset->SourceSequences)
		{
			if (!Seq)
			{
				continue;
			}
			++SequenceCount;

			const float PlayLength = Seq->GetPlayLength();
			if (PlayLength <= 0.f)
			{
				UE_LOG(LogSomaLipsyncEditor, Warning,
					TEXT("BuildDatabaseIndex: '%s' has zero play length, skipping."),
					*Seq->GetName());
				continue;
			}

			const FString Label = Seq->GetName();
			const int32 NumSamples = FMath::Max(1, FMath::FloorToInt(PlayLength * SampleRate) + 1);

			for (int32 i = 0; i < NumSamples; ++i)
			{
				const float Time = FMath::Min(static_cast<float>(i) * DeltaT, PlayLength);

				FLipsyncPoseSample Sample;
				Sample.Sequence = Seq;
				Sample.Time = Time;
				Sample.SourceLabel = Label;

				if (!FSomaLipsyncBoneExtractor::ExtractMouthPose(Seq, Time, Bones, Sample.MouthPose))
				{
					continue;
				}

				// Bake the trajectory-matching window. If any station offset
				// falls outside [0, PlayLength], skip the whole window: cleaner
				// than padding, and we just lose ~300 ms of usable starts/ends
				// per clip. The runtime matcher will fall back to MouthPose for
				// these samples.
				Sample.Window.Stations.SetNum(SomaLipsyncTraj::NumStations);
				bool bWindowValid = true;
				for (int32 k = 0; k < SomaLipsyncTraj::NumStations; ++k)
				{
					const float StationTime = Time + SomaLipsyncTraj::StationOffsetsSeconds[k];
					if (StationTime < 0.f || StationTime > PlayLength)
					{
						bWindowValid = false;
						break;
					}
					if (!FSomaLipsyncBoneExtractor::ExtractMouthPose(Seq, StationTime, Bones, Sample.Window.Stations[k]))
					{
						bWindowValid = false;
						break;
					}
				}
				Sample.Window.bValid = bWindowValid;
				if (!bWindowValid)
				{
					Sample.Window.Stations.Reset();
					++WindowsInvalid;
				}
				else
				{
					++WindowsValid;
				}

				Asset->Samples.Add(MoveTemp(Sample));
			}
		}

		Asset->MarkPackageDirty();

		UE_LOG(LogSomaLipsyncEditor, Display,
			TEXT("BuildDatabaseIndex: built %d samples from %d sequences on '%s' (rate=%.1f fps); windows valid=%d invalid=%d"),
			Asset->Samples.Num(), SequenceCount, *Asset->GetName(), SampleRate,
			WindowsValid, WindowsInvalid);
	}
}

void FSomaLipsyncEditorModule::StartupModule()
{
	FSomaLipsyncEditorBridge& Bridge = FSomaLipsyncEditorBridge::Get();
	Bridge.CaptureViseme = &CaptureFromSourceAnims;
	Bridge.BuildDatabaseIndex = &BuildDatabaseIndex;

	UE_LOG(LogSomaLipsyncEditor, Display, TEXT("soma_lipsyncEditor: editor bridge installed."));
}

void FSomaLipsyncEditorModule::ShutdownModule()
{
	FSomaLipsyncEditorBridge& Bridge = FSomaLipsyncEditorBridge::Get();
	Bridge.CaptureViseme = nullptr;
	Bridge.BuildDatabaseIndex = nullptr;
}

IMPLEMENT_MODULE(FSomaLipsyncEditorModule, soma_lipsyncEditor)

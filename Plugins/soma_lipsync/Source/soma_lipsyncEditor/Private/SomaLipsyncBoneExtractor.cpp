#include "SomaLipsyncBoneExtractor.h"

#include "Animation/AnimSequence.h"
#include "AnimationBlueprintLibrary.h"
#include "SomaLipsyncEditorModule.h"

bool FSomaLipsyncBoneExtractor::ExtractBoneTransform(UAnimSequence* Sequence, FName BoneName, float Time, FTransform& OutTransform)
{
	OutTransform = FTransform::Identity;

	if (!Sequence || BoneName.IsNone())
	{
		return false;
	}

	const USkeleton* Skeleton = Sequence->GetSkeleton();
	if (!Skeleton)
	{
		UE_LOG(LogSomaLipsyncEditor, Warning,
			TEXT("ExtractBoneTransform: sequence '%s' has no skeleton"),
			*Sequence->GetName());
		return false;
	}

	if (Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName) == INDEX_NONE)
	{
		UE_LOG(LogSomaLipsyncEditor, Warning,
			TEXT("ExtractBoneTransform: bone '%s' not found on skeleton '%s' (sequence '%s')"),
			*BoneName.ToString(), *Skeleton->GetName(), *Sequence->GetName());
		return false;
	}

	const float ClampedTime = FMath::Clamp(Time, 0.f, Sequence->GetPlayLength());

	// UAnimationBlueprintLibrary::GetBonePoseForTime returns the parent-relative
	// (local) transform of BoneName at ClampedTime, which matches the coordinate
	// space mandated by PDF section 4.1.
	UAnimationBlueprintLibrary::GetBonePoseForTime(Sequence, BoneName, ClampedTime, /*bExtractRootMotion=*/false, OutTransform);
	return true;
}

bool FSomaLipsyncBoneExtractor::ExtractMouthPose(UAnimSequence* Sequence, float Time, FLipsyncMouthPose& OutPose)
{
	const TArray<FName> DefaultBones = {
		SomaLipsyncBones::LowLip,
		SomaLipsyncBones::UpLip,
		SomaLipsyncBones::LCorner,
		SomaLipsyncBones::RCorner
	};
	return ExtractMouthPose(Sequence, Time, DefaultBones, OutPose);
}

bool FSomaLipsyncBoneExtractor::ExtractMouthPose(UAnimSequence* Sequence, float Time, const TArray<FName>& BoneNames, FLipsyncMouthPose& OutPose)
{
	OutPose = FLipsyncMouthPose();

	if (!Sequence)
	{
		return false;
	}

	auto AssignByName = [&OutPose](FName Name, const FTransform& Xfm) -> bool
	{
		if (Name == SomaLipsyncBones::LowLip)   { OutPose.LowLip = Xfm; return true; }
		if (Name == SomaLipsyncBones::UpLip)    { OutPose.UpLip = Xfm; return true; }
		if (Name == SomaLipsyncBones::LCorner)  { OutPose.LeftCorner = Xfm; return true; }
		if (Name == SomaLipsyncBones::RCorner)  { OutPose.RightCorner = Xfm; return true; }
		return false;
	};

	bool bAnyExtracted = false;
	for (const FName& Bone : BoneNames)
	{
		FTransform Xfm = FTransform::Identity;
		if (ExtractBoneTransform(Sequence, Bone, Time, Xfm))
		{
			if (AssignByName(Bone, Xfm))
			{
				bAnyExtracted = true;
			}
			else
			{
				UE_LOG(LogSomaLipsyncEditor, Verbose,
					TEXT("ExtractMouthPose: bone '%s' is not a recognized mouth bone slot, skipping."),
					*Bone.ToString());
			}
		}
	}

	return bAnyExtracted;
}

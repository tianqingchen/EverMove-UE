#pragma once

#include "CoreMinimal.h"
#include "SomaLipsyncTypes.h"

class UAnimSequence;

/**
 * Editor-only helpers used by both UVisemePoseTargetMap::CaptureFromSourceAnims
 * and ULipsyncMotionDatabase::BuildIndex so the two paths produce data in the
 * exact same coordinate space (parent-relative, per PDF section 4.1).
 */
class SOMA_LIPSYNCEDITOR_API FSomaLipsyncBoneExtractor
{
public:
	/**
	 * Sample the parent-relative transform of BoneName in Sequence at Time.
	 * Returns FTransform::Identity and false if the bone or asset is invalid.
	 */
	static bool ExtractBoneTransform(UAnimSequence* Sequence, FName BoneName, float Time, FTransform& OutTransform);

	/** Convenience helper: fill an FLipsyncMouthPose at Time using the standard 4 mouth bones. */
	static bool ExtractMouthPose(UAnimSequence* Sequence, float Time, FLipsyncMouthPose& OutPose);

	/** Generic version that uses an explicit bone-name list (matches ULipsyncMotionDatabase::RequiredBones). */
	static bool ExtractMouthPose(UAnimSequence* Sequence, float Time, const TArray<FName>& BoneNames, FLipsyncMouthPose& OutPose);
};

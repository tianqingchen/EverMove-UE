#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "SomaLipsyncTypes.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSomaLipsync, Log, All);

class UAnimSequence;
class UVisemePoseTargetMap;
class ULipsyncMotionDatabase;

/**
 * Hook installed by soma_lipsyncEditor at module startup so the runtime data
 * assets can call back into editor-only bone extraction without taking a hard
 * dependency on UnrealEd. All callbacks are no-ops in cooked builds.
 */
struct SOMA_LIPSYNC_API FSomaLipsyncEditorBridge
{
	/** void(UVisemePoseTargetMap* Asset) */
	TFunction<void(UVisemePoseTargetMap*)> CaptureViseme;

	/** void(ULipsyncMotionDatabase* Asset) */
	TFunction<void(ULipsyncMotionDatabase*)> BuildDatabaseIndex;

	static FSomaLipsyncEditorBridge& Get();
};

class FSomaLipsyncModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

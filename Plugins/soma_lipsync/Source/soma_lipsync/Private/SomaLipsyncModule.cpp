#include "SomaLipsyncModule.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogSomaLipsync);

FSomaLipsyncEditorBridge& FSomaLipsyncEditorBridge::Get()
{
	static FSomaLipsyncEditorBridge Bridge;
	return Bridge;
}

void FSomaLipsyncModule::StartupModule()
{
}

void FSomaLipsyncModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FSomaLipsyncModule, soma_lipsync)

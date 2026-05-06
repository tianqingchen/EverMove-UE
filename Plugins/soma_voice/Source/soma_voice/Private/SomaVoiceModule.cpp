#include "Modules/ModuleManager.h"

class FSomaVoiceModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
};

IMPLEMENT_MODULE(FSomaVoiceModule, soma_voice)

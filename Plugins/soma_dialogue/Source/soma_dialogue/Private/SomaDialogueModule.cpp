#include "Modules/ModuleManager.h"

class FSomaDialogueModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
};

IMPLEMENT_MODULE(FSomaDialogueModule, soma_dialogue)

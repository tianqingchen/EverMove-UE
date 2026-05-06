#include "Modules/ModuleManager.h"

class FSomaStorageModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FSomaStorageModule, soma_storage)

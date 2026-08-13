
#pragma once
#include "e_utils.h"


class ModelLoadHooks {
public:
    static ModelLoadHooks& Instance() {
        static ModelLoadHooks instance;
        return instance;
    }

    void Initialize();
    void ReloadModelsAfterSettings();
    void Shutdown();
private:
    // Hook objects
    Detouring::Hook m_DrawModelExecute_hook;

};

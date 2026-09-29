#pragma once

#include "memory_backend.h"

// The overlay is deliberately blocked at this point: no process scan, driver
// launch, or game-memory access starts until the user picks a transport.
bool StartupModeReady();
bool StartupModeSelect(memaccess::AccessMode mode);
const char* StartupModeError();
void RenderStartupModeMenu();
void StartupModeShutdown();

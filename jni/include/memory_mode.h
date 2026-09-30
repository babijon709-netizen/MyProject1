#pragma once

// Selects the transport used for game-memory operations. The choice is made
// before the rendering and attach threads are started so a Reader never swaps
// transports while it is servicing a frame.
namespace memio {

enum class Backend {
    ProcMem,
    KernelDriver,
};

Backend backend();
const char* backend_name();

// Parses --proc-mem/--kernel (and MEMORY_BACKEND), or shows a small terminal
// menu when the executable has an interactive stdin. In non-interactive
// launches the historical /proc backend remains the safe default.
bool choose_backend(int argc, char* argv[]);

}  // namespace memio

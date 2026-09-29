#pragma once

#include <string>
#include <vector>

namespace kernel_mode {

// Handles --mode/--driver command-line options and, when attached to a terminal,
// displays the launch-mode menu. Returns false when kernel mode was requested
// but no matching supplied loader could be started successfully.
bool configure_before_start(int argc, char* argv[]);

const char* selected_driver();
const char* diagnostic();

}  // namespace kernel_mode

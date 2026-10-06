#pragma once

#include <sys/types.h>

namespace fake_lag {

enum class Status { Off, Waiting, Active, Failed };

// Applies the original INPUT UDP toggle cycle. The PID is retained in the API
// for the existing call site but the firewall rule intentionally is not UID-scoped.
void update(bool enabled, int percent, pid_t game_pid);
void shutdown();
Status status();
inline int clamp_percent(int percent) {
    return percent < 1 ? 1 : (percent > 100 ? 100 : percent);
}

} // namespace fake_lag

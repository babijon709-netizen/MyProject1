#pragma once

#include <sys/types.h>

namespace fake_lag {

enum class Status { Off, Waiting, Active, Failed };

// Updates the session-scoped fake-lag controller. Calls are cheap when the
// settings and target process are unchanged; iptables work runs off-thread.
void update(bool enabled, int percent, pid_t game_pid);
void shutdown();
Status status();
inline int clamp_percent(int percent) {
    return percent < 10 ? 10 : (percent > 70 ? 70 : percent);
}

} // namespace fake_lag

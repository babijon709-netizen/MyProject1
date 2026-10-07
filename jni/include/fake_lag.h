#pragma once

namespace fake_lag {

enum class Status { Off, Waiting, Active, Failed };

inline int clamp_percent(int percent) {
    return percent < 1 ? 1 : (percent > 100 ? 100 : percent);
}

inline int blocked_ms(int percent) {
    return (1000 * clamp_percent(percent)) / 100;
}

inline int unblocked_ms(int percent) {
    return 1000 - blocked_ms(percent);
}

// Mirrors the Drive implementation: non-loopback incoming UDP is toggled.
void update(bool enabled, int percent);
void shutdown();
Status status();

} // namespace fake_lag

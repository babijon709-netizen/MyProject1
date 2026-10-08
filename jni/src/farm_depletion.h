#pragma once

#include <cmath>

namespace farm_depletion {

constexpr float kTerminalHealthRatio = 0.02f;

inline bool health_terminal(float current, float maximum) {
    if (!std::isfinite(current) || current < 0.0f) return false;
    if (current <= 0.0f) return true;
    return std::isfinite(maximum) && maximum > 0.0f &&
           current <= maximum * kTerminalHealthRatio;
}

inline bool fraction_confirmed(float fraction, int& consecutive_frames,
                               float threshold, int required_frames) {
    if (!std::isfinite(fraction) || fraction < 0.0f || fraction >= threshold) {
        consecutive_frames = 0;
        return false;
    }
    if (required_frames <= 0) return true;
    if (consecutive_frames < required_frames) ++consecutive_frames;
    return consecutive_frames >= required_frames;
}

} // namespace farm_depletion

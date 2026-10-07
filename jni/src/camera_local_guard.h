#pragma once

#include <cmath>

namespace camera_local_guard {

inline bool confirm_mismatch(bool far, unsigned& streak, unsigned required = 2) {
    if (!far) {
        streak = 0;
        return false;
    }
    if (required == 0) return true;
    if (streak < required) ++streak;
    return streak >= required;
}

inline bool within(float ax, float ay, float az,
                   float bx, float by, float bz,
                   float max_distance) {
    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az) ||
        !std::isfinite(bx) || !std::isfinite(by) || !std::isfinite(bz) ||
        !std::isfinite(max_distance) || max_distance <= 0.f)
        return false;
    const double dx = static_cast<double>(ax) - bx;
    const double dy = static_cast<double>(ay) - by;
    const double dz = static_cast<double>(az) - bz;
    const double limit = max_distance;
    return dx * dx + dy * dy + dz * dz <= limit * limit;
}

} // namespace camera_local_guard

#pragma once

#include <cmath>

namespace espsnapshot {

inline bool within_age(double now, double sampled_at, double max_age) {
    if (!std::isfinite(now) || !std::isfinite(sampled_at) ||
        !std::isfinite(max_age) || sampled_at <= 0.0 || now < sampled_at ||
        max_age < 0.0)
        return false;
    return now - sampled_at <= max_age;
}

inline bool due(double now, double sampled_at, double interval) {
    if (!std::isfinite(now) || !std::isfinite(sampled_at) ||
        !std::isfinite(interval) || sampled_at <= 0.0 || now < sampled_at ||
        interval < 0.0)
        return true;
    return now - sampled_at >= interval;
}

}  // namespace espsnapshot

#pragma once

#include <cmath>
#include <cstdint>

namespace manes::aim_visibility {

constexpr float kMaxSensorTimeSkewSeconds = 0.05f;
constexpr float kSensorClockEpsilon = 0.0001f;
constexpr double kMaxSensorSilenceSeconds = 0.05;

struct SensorFreshness {
    std::uint64_t player = 0;
    std::uint64_t collider = 0;
    float last_rect_time = 0.f;
    double last_rect_observed_at = 0.0;
    bool initialized = false;
    bool rect_advanced = false;
};

inline bool sensor_timestamps_show_line_of_sight(float last_seen_time,
                                                 float last_rect_time) {
    if (!std::isfinite(last_seen_time) || !std::isfinite(last_rect_time) ||
        last_seen_time <= 0.f || last_rect_time <= 0.f)
        return false;
    return std::fabs(last_seen_time - last_rect_time) <= kMaxSensorTimeSkewSeconds;
}

inline bool sensor_sample_is_current(SensorFreshness& freshness,
                                     std::uint64_t player,
                                     std::uint64_t collider,
                                     float last_seen_time,
                                     float last_rect_time,
                                     double observed_at) {
    if (player < 0x10000 || collider < 0x10000 ||
        !std::isfinite(last_seen_time) || !std::isfinite(last_rect_time) ||
        !std::isfinite(observed_at) || last_seen_time <= 0.f || last_rect_time <= 0.f) {
        freshness = {};
        return false;
    }

    if (!freshness.initialized || freshness.player != player ||
        freshness.collider != collider) {
        freshness = {player, collider, last_rect_time, observed_at, true, false};
        return false;
    }

    if (std::fabs(last_rect_time - freshness.last_rect_time) > kSensorClockEpsilon) {
        freshness.last_rect_time = last_rect_time;
        freshness.last_rect_observed_at = observed_at;
        freshness.rect_advanced = true;
    }

    if (!freshness.rect_advanced || observed_at < freshness.last_rect_observed_at ||
        observed_at - freshness.last_rect_observed_at > kMaxSensorSilenceSeconds)
        return false;

    return sensor_timestamps_show_line_of_sight(last_seen_time, last_rect_time);
}

} // namespace manes::aim_visibility

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace hitsound {

struct HealthSample {
    std::uint64_t id = 0;
    float health = -1.0f;
    bool ally = false;
};

class Tracker {
public:
    void reset() {
        health_.clear();
        shot_known_ = false;
        last_shot_time_ = 0.0f;
        last_shot_seen_ = 0.0;
        shot_deadline_ = 0.0;
    }

    void observe_shot(bool readable, float game_time, double now) {
        if (!std::isfinite(now)) return;
        if (!readable || !std::isfinite(game_time)) {
            if (shot_known_ && now >= last_shot_seen_ &&
                now - last_shot_seen_ > kShotReadTimeout) {
                shot_known_ = false;
                shot_deadline_ = 0.0;
            }
            return;
        }

        if (!shot_known_ || now < last_shot_seen_ ||
            now - last_shot_seen_ > kShotReadTimeout) {
            shot_known_ = true;
            last_shot_time_ = game_time;
            last_shot_seen_ = now;
            shot_deadline_ = 0.0;
            return;
        }

        last_shot_seen_ = now;
        if (game_time < last_shot_time_ - kShotTimeEpsilon) {
            last_shot_time_ = game_time;
            shot_deadline_ = 0.0;
            health_.clear();
            return;
        }
        if (game_time > last_shot_time_ + kShotTimeEpsilon) {
            last_shot_time_ = game_time;
            shot_deadline_ = now + kShotWindowSeconds;
        }
    }

    bool observe_health(const HealthSample* samples, std::size_t count,
                       bool transient, double now) {
        if (transient || !std::isfinite(now)) return false;

        bool hit = false;
        for (std::size_t i = 0; i < count; ++i) {
            const HealthSample& sample = samples[i];
            if (!sample.id || !std::isfinite(sample.health) ||
                sample.health < 0.0f || sample.health > kMaxHealth)
                continue;

            auto [it, inserted] = health_.try_emplace(
                sample.id, HealthState{sample.health, now});
            if (inserted) continue;

            if (!sample.ally && it->second.value - sample.health > kMinHealthDrop &&
                shot_deadline_ > 0.0 && now <= shot_deadline_)
                hit = true;

            it->second.value = sample.health;
            it->second.seen_at = now;
        }

        for (auto it = health_.begin(); it != health_.end();) {
            if (now >= it->second.seen_at &&
                now - it->second.seen_at > kSampleLifetimeSeconds)
                it = health_.erase(it);
            else
                ++it;
        }
        return hit;
    }

private:
    struct HealthState {
        float value;
        double seen_at;
    };

    static constexpr float kMaxHealth = 100000.0f;
    static constexpr float kMinHealthDrop = 0.25f;
    static constexpr float kShotTimeEpsilon = 0.0001f;
    static constexpr double kShotWindowSeconds = 0.75;
    static constexpr double kShotReadTimeout = 2.0;
    static constexpr double kSampleLifetimeSeconds = 3.0;

    std::unordered_map<std::uint64_t, HealthState> health_;
    bool shot_known_ = false;
    float last_shot_time_ = 0.0f;
    double last_shot_seen_ = 0.0;
    double shot_deadline_ = 0.0;
};

}  // namespace hitsound

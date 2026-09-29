#pragma once

// Player-list/position health is intentionally independent from camera and
// world-marker publication. A valid camera frame must never hide a dead player
// pipeline indefinitely.
struct PlayerEspWatchdogAction {
    bool reset_resolvers = false;
    bool deep_recovery = false;
};

class PlayerEspWatchdog {
public:
    static constexpr double kResolverResetSeconds = 0.75;
    static constexpr double kResetRepeatSeconds = 1.50;
    static constexpr double kDeepRecoverySeconds = 5.0;

    PlayerEspWatchdogAction observe(bool player_data_ok, double now) {
        if (player_data_ok) {
            reset();
            return {};
        }
        if (!failing_) {
            failing_ = true;
            failed_since_ = now;
            // Permit the first soft reset as soon as the settle window ends.
            last_reset_at_ = now - kResetRepeatSeconds;
            return {};
        }

        PlayerEspWatchdogAction action{};
        const double elapsed = now - failed_since_;
        if (elapsed >= kResolverResetSeconds &&
            now - last_reset_at_ >= kResetRepeatSeconds) {
            last_reset_at_ = now;
            action.reset_resolvers = true;
        }
        if (!deep_recovery_issued_ && elapsed >= kDeepRecoverySeconds) {
            deep_recovery_issued_ = true;
            action.reset_resolvers = true;
            action.deep_recovery = true;
        }
        return action;
    }

    void reset() {
        failing_ = false;
        deep_recovery_issued_ = false;
        failed_since_ = 0.0;
        last_reset_at_ = 0.0;
    }

    bool failing() const { return failing_; }
    double failed_since() const { return failed_since_; }

private:
    bool failing_ = false;
    bool deep_recovery_issued_ = false;
    double failed_since_ = 0.0;
    double last_reset_at_ = 0.0;
};

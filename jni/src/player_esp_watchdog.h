#pragma once

// Player-list/position health is intentionally independent from camera and
// world-marker publication. A valid camera frame must never hide a dead player
// pipeline indefinitely.
struct PlayerEspWatchdogAction {
    bool reset_resolvers = false;
    bool request_reattach = false;
};

class PlayerEspWatchdog {
public:
    static constexpr double kResolverResetSeconds = 0.75;
    static constexpr double kResetRepeatSeconds = 1.50;
    static constexpr double kReattachSeconds = 5.0;

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
        if (!reattach_requested_ && elapsed >= kReattachSeconds) {
            reattach_requested_ = true;
            action.reset_resolvers = true;
            action.request_reattach = true;
        }
        return action;
    }

    void reset() {
        failing_ = false;
        reattach_requested_ = false;
        failed_since_ = 0.0;
        last_reset_at_ = 0.0;
    }

    bool failing() const { return failing_; }
    double failed_since() const { return failed_since_; }

private:
    bool failing_ = false;
    bool reattach_requested_ = false;
    double failed_since_ = 0.0;
    double last_reset_at_ = 0.0;
};

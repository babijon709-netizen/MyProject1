#pragma once

// Small, testable publication cache used only to bridge transient player-list
// gaps during death/respawn.  Live frames always win.  Cached entries are
// visual-only: all aim points are removed before they are republished.
#include "game.h"
#include <vector>

class EspContinuity {
    std::vector<EspBox> players_;
    double remembered_at_ = 0.0;

    static bool has_player(const std::vector<EspBox>& frame) {
        for (const EspBox& box : frame) if (!box.is_bot) return true;
        return false;
    }

    static void disable_aim(EspBox& box) {
        box.aim_source = 0;
        for (int i = 0; i < 3; ++i) {
            box.aim_valid[i] = false;
            box.aim_world_valid[i] = false;
            box.aim_yaw[i] = 0.0F;
            box.aim_pitch[i] = 0.0F;
            for (int axis = 0; axis < 3; ++axis) box.aim_world[i][axis] = 0.0F;
        }
    }

public:
    // Мост закрывает РВАНЫЙ кадр, а не всю смерть. Прежние 10 секунд
    // означали, что после гибели боксы буквально зависали на месте, пока
    // игрок не возродится (жалоба 29.09). Полсекунды не видно глазом, но
    // хватает на серию неудачных чтений списка игроков.
    static constexpr double kBridgeSeconds = 0.5;

    void clear() {
        players_.clear();
        remembered_at_ = 0.0;
    }

    bool has_snapshot() const { return !players_.empty(); }

    // healthy=true means the player population and a real local-player
    // position were both available.  Such a frame replaces the snapshot,
    // including with an empty set (the last enemy really left).  A transient
    // frame is returned untouched when it already contains live player boxes;
    // only a completely empty player publication is bridged.
    std::vector<EspBox> publish(std::vector<EspBox> frame, bool healthy, double now) {
        if (healthy) {
            players_.clear();
            for (const EspBox& box : frame) if (!box.is_bot) players_.push_back(box);
            remembered_at_ = now;
            return frame;
        }
        if (has_player(frame) || players_.empty() || now < remembered_at_ ||
            now - remembered_at_ > kBridgeSeconds) return frame;

        frame.reserve(frame.size() + players_.size());
        for (EspBox box : players_) {
            disable_aim(box);
            frame.push_back(box);
        }
        return frame;
    }
};

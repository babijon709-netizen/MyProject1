#pragma once
// Movement / Jump memory controller.
// Heap-only object-data implementation: no instruction writes, no library patches,
// no hooks in libil2cpp.so or libunity.so. Only writes to CharacterSpeedConfig in the heap.
#include "viewmodel.h"
#include "game_offsets_active.h"

namespace movement {
template<class IO> bool pointer(IO& io, uint64_t a, uint64_t& value) {
    if (!io.read(a, value)) return false;
    value &= 0x00ffffffffffffffULL;
    return true;
}

inline bool valid_jump_height(float v) {
    return viewmodel::finite(v) && v >= 0.2f && v <= 20.0f;
}

struct SavedJump {
    uint64_t address = 0;
    float original = 0.f;
    bool owned = false;
};

template<class IO> class Controller {
    IO& io_;
    uint64_t session_ = 0;
    uint64_t player_ = 0;
    uint64_t speed_config_ = 0;
    SavedJump saved_{};

public:
    viewmodel::Status status = viewmodel::Status::Off;

    explicit Controller(IO& io) : io_(io) {}

    bool has_state() const { return saved_.owned; }

    void reset(bool do_restore = false) {
        if (do_restore && saved_.owned) {
            restore();
        }
        saved_ = {};
        session_ = 0;
        player_ = 0;
        speed_config_ = 0;
        status = viewmodel::Status::Off;
    }

    bool restore() {
        if (!saved_.owned || !saved_.address) {
            saved_ = {};
            return true;
        }
        float now = 0.f;
        if (!io_.read(saved_.address, now)) {
            saved_ = {};
            return false;
        }
        if (saved_.original > 0.f && now != saved_.original) {
            io_.write_heap(saved_.address, saved_.original);
        }
        saved_ = {};
        return true;
    }

    // Resolves CharacterSpeedConfig from player
    uint64_t resolve_speed_config(uint64_t player) {
        if (player < 0x10000) return 0;

        // Verify player is local PlayerManager
        uint64_t p = 0;
        uint8_t local = 0;
        if (!io_.named(player, "PlayerManager", "Oxide")) return 0;
        if (pointer(io_, player + 0x40, p) && io_.named(p, "NetworkIdentity", "Mirror")) {
            if (io_.read(p + 0x4a, local) && local != 1) return 0;
        }

        // Route 1: via KCC reference
        uint64_t ref = 0;
        if (pointer(io_, player + go_active::PLAYER_KCC_REFERENCE, ref) && ref >= 0x10000) {
            // Check if ref is wrapper with backref at 0x10
            uint64_t back = 0;
            if (pointer(io_, ref + 0x10, back) && back == player) {
                uint64_t cfg = 0;
                if (pointer(io_, ref + 0x20, cfg) && cfg >= 0x10000) {
                    if (io_.named(cfg, "CharacterSpeedConfig", "HyperHug.Games.Oxide.Features.Player")) {
                        return cfg;
                    }
                }
            }

            // Check if ref is KCC itself (KCC.player at 0x78)
            if (pointer(io_, ref + go_active::KCC_PLAYER_BACKREF, back) && back == player) {
                uint64_t cfg = 0;
                if (pointer(io_, ref + go_active::KCC_CHARACTER_SPEED_CONFIG, cfg) && cfg >= 0x10000) {
                    if (io_.named(cfg, "CharacterSpeedConfig", "HyperHug.Games.Oxide.Features.Player")) {
                        return cfg;
                    }
                }
            }

            // Probe child pointers in ref
            for (uint64_t off = 0x08; off <= 0x60; off += 8) {
                uint64_t cand = 0;
                if (pointer(io_, ref + off, cand) && cand >= 0x10000) {
                    if (pointer(io_, cand + go_active::KCC_PLAYER_BACKREF, back) && back == player) {
                        uint64_t cfg = 0;
                        if (pointer(io_, cand + go_active::KCC_CHARACTER_SPEED_CONFIG, cfg) && cfg >= 0x10000) {
                            if (io_.named(cfg, "CharacterSpeedConfig", "HyperHug.Games.Oxide.Features.Player")) {
                                return cfg;
                            }
                        }
                    }
                }
            }
        }

        // Route 2: scan player fields for KCC
        for (uint64_t off = 0x68; off <= 0x2C8; off += 8) {
            uint64_t cand = 0;
            if (pointer(io_, player + off, cand) && cand >= 0x10000) {
                uint64_t back = 0;
                if (pointer(io_, cand + go_active::KCC_PLAYER_BACKREF, back) && back == player) {
                    uint64_t cfg = 0;
                    if (pointer(io_, cand + go_active::KCC_CHARACTER_SPEED_CONFIG, cfg) && cfg >= 0x10000) {
                        if (io_.named(cfg, "CharacterSpeedConfig", "HyperHug.Games.Oxide.Features.Player")) {
                            return cfg;
                        }
                    }
                }
            }
        }

        return 0;
    }

    void tick(uint64_t session, uint64_t player, const ViewmodelSettings& settings) {
        if (session != session_ || player != player_) {
            if (saved_.owned) restore();
            session_ = session;
            player_ = player;
            speed_config_ = 0;
        }

        if (!settings.high_jump) {
            if (saved_.owned) {
                restore();
            }
            status = viewmodel::Status::Off;
            return;
        }

        if (!player) {
            status = saved_.owned ? viewmodel::Status::Failed : viewmodel::Status::Waiting;
            return;
        }

        if (!speed_config_) {
            speed_config_ = resolve_speed_config(player);
            if (!speed_config_) {
                status = viewmodel::Status::Waiting;
                return;
            }
        }

        const uint64_t jump_addr = speed_config_ + go_active::CHARACTERSPEEDCONFIG_JUMP_HEIGHT;
        float now = 0.f;
        if (!io_.read(jump_addr, now) || !valid_jump_height(now)) {
            speed_config_ = 0; // invalidate and retry
            status = viewmodel::Status::Failed;
            return;
        }

        const float target = viewmodel::bounded(settings.jump_height, 1.0f, 5.0f, 2.5f);

        // If not owned yet, capture the original value
        if (!saved_.owned) {
            saved_.address = jump_addr;
            saved_.original = now;
            saved_.owned = true;
        }

        // If value doesn't match target, write target
        if (now != target) {
            if (!io_.write_heap(jump_addr, target)) {
                status = viewmodel::Status::Failed;
                return;
            }
            float check = 0.f;
            if (!io_.read(jump_addr, check) || check != target) {
                status = viewmodel::Status::Failed;
                return;
            }
        }

        status = viewmodel::Status::Active;
    }
};
} // namespace movement

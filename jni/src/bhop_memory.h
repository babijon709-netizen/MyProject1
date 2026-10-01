#pragma once
// Test B-Hop: keep the game's normal jump input, height and physics, but remove
// the local jump-endurance recovery delay. Only the three named fields of the
// validated CharacterSpeedConfig asset are touched; originals are restored.
#include "viewmodel.h"
#include "game_offsets_active.h"
#include <cstdint>

namespace bhop {
template<class IO> bool pointer(IO& io, uint64_t address, uint64_t& value) {
    if (!io.read(address, value)) return false;
    value &= 0x00ffffffffffffffULL;
    return true;
}

struct Scalar {
    uint64_t address = 0;
    float original = 0.f;
    bool owned = false;

    template<class IO> bool apply_zero(IO& io, uint64_t addr) {
        float value = 0.f;
        if (!io.read(addr, value) || !viewmodel::finite(value) || value < 0.f || value > 60.f)
            return false;
        if (!owned || address != addr) { address = addr; original = value; owned = true; }
        if (value == 0.f) return true;
        if (!io.write_heap(addr, 0.f) || !io.read(addr, value) || value != 0.f) return false;
        return true;
    }
    template<class IO> bool restore(IO& io) {
        if (!owned) return true;
        float value = 0.f;
        if (!io.read(address, value)) return false;
        if (value == 0.f && original != 0.f && !io.write_heap(address, original)) return false;
        *this = {};
        return true;
    }
};

template<class IO> class Controller {
    IO& io_;
    uint64_t session_ = 0, config_ = 0;
    Scalar cost_{}, pause_{}, idle_pause_{};

    bool restore() {
        const bool a = cost_.restore(io_);
        const bool b = pause_.restore(io_);
        const bool c = idle_pause_.restore(io_);
        return a && b && c;
    }
    bool valid_kcc(uint64_t kcc, uint64_t player) {
        uint64_t back = 0;
        return kcc >= 0x10000 && io_.named(kcc, "KCC", "HyperHug.Games.Oxide.Features.Player") &&
               pointer(io_, kcc + go_active::KCC_PLAYER_BACKREF, back) && back == player;
    }
    uint64_t resolve_kcc(uint64_t player) {
        uint64_t ref = 0;
        if (!pointer(io_, player + go_active::PLAYER_KCC_REFERENCE, ref) || !ref) return 0;
        if (valid_kcc(ref, player)) return ref;
        // kccReference is commonly a lazy wrapper around the live component.
        for (uint64_t off = 8; off <= 0x60; off += 8) {
            uint64_t candidate = 0;
            if (pointer(io_, ref + off, candidate) && valid_kcc(candidate, player)) return candidate;
        }
        return 0;
    }
public:
    viewmodel::Status status = viewmodel::Status::Off;
    explicit Controller(IO& io) : io_(io) {}
    bool has_state() const { return cost_.owned || pause_.owned || idle_pause_.owned; }
    bool reset(bool restore_originals) {
        const bool ok = !restore_originals || restore();
        cost_ = {}; pause_ = {}; idle_pause_ = {};
        session_ = config_ = 0; status = viewmodel::Status::Off;
        return ok;
    }
    void tick(uint64_t session, uint64_t player, bool enabled) {
        using viewmodel::Status;
        if (session != session_) { reset(false); session_ = session; }
        if (!enabled) { status = restore() ? Status::Off : Status::Failed; return; }
        status = Status::Waiting;
        const uint64_t kcc = resolve_kcc(player);
        uint64_t config = 0;
        if (!kcc || !pointer(io_, kcc + go_active::KCC_SPEED_CONFIG, config) ||
            !io_.named(config, "CharacterSpeedConfig", "HyperHug.Games.Oxide.Features.Player"))
            return;
        if (config_ && config_ != config && !restore()) { status = Status::Failed; return; }
        config_ = config;
        const bool ok = cost_.apply_zero(io_, config + go_active::SPEEDCFG_JUMP_ENDURANCE_COST) &&
                        pause_.apply_zero(io_, config + go_active::SPEEDCFG_JUMP_REGEN_PAUSE) &&
                        idle_pause_.apply_zero(io_, config + go_active::SPEEDCFG_JUMP_REGEN_IDLE_PAUSE);
        status = ok ? Status::Active : Status::Failed;
    }
};
} // namespace bhop

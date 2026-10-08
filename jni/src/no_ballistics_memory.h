#pragma once
// "Без баллистики": зануляет gravity/airDrag/underwaterDrag в активном
// BallisticProfile и в ShaftedProjectile-префабе выбранного оружия. Значения
// применяются только к объектам из подтверждённой цепочки локального ствола;
// оригиналы восстанавливаются при выключении, смене оружия или отключении.
#include "viewmodel.h"
#include "game_offsets_active.h"
#include "fast_fire_memory.h"
#include "weapon_tuning_memory.h"
#include <array>
#include <cstdint>

namespace no_ballistics {
using weapon_tuning::Identity;
using weapon_tuning::Life;
using weapon_tuning::pointer;
using weapon_tuning::kWeapons;
using fast_fire::Kind;

inline constexpr const char* kBallistics =
    "HyperHug.Games.Oxide.Features.Weapons.Ballistics";

inline bool has_profile(Kind kind) {
    return kind == Kind::Hitscan || kind == Kind::Crossbow ||
           kind == Kind::RocketLauncher;
}
inline bool has_projectile(Kind kind) {
    return kind == Kind::Bow || kind == Kind::Crossbow ||
           kind == Kind::RocketLauncher || kind == Kind::ExplosiveCharge ||
           kind == Kind::Snowball || kind == Kind::Spear;
}
inline std::uint64_t config_offset(Kind kind) {
    using namespace go_active;
    switch (kind) {
        case Kind::Hitscan:        return FPHITSCAN_CONFIG;
        case Kind::Crossbow:       return FPCROSSBOW_CONFIG;
        case Kind::RocketLauncher: return FPROCKETLAUNCHER_CONFIG;
        default:                   return 0;
    }
}
inline std::uint64_t projectile_offset(Kind kind) {
    using namespace go_active;
    switch (kind) {
        case Kind::Bow:             return FPBOW_PROJECTILE_PREFAB;
        case Kind::Crossbow:        return FPCROSSBOW_PROJECTILE_PREFAB;
        case Kind::RocketLauncher:  return FPROCKETLAUNCHER_PROJECTILE_PREFAB;
        case Kind::ExplosiveCharge: return FPEXPLOSIVECHARGE_PROJECTILE_PREFAB;
        case Kind::Snowball:        return FPSNOWBALL_PROJECTILE_PREFAB;
        case Kind::Spear:           return FPSPEAR_PROJECTILE_PREFAB;
        default:                    return 0;
    }
}

struct Context {
    Identity player, fp, weapon, config, profile, projectile;
    Kind kind = Kind::Unknown;
    bool operator==(const Context& b) const {
        return player == b.player && fp == b.fp && weapon == b.weapon &&
               config == b.config && profile == b.profile &&
               projectile == b.projectile && kind == b.kind;
    }
};
struct Scalar {
    float original = 0.f;
    bool owned = false;
};
struct DebugInfo {
    Kind kind = Kind::Unknown, probe_kind = Kind::Unknown;
    uint64_t weapon = 0, config = 0, profile = 0, projectile = 0;
    uint64_t probe_player = 0, probe_fp = 0, probe_weapon = 0;
    uint64_t probe_config = 0, probe_profile = 0, probe_projectile = 0;
    uint8_t target_mask = 0, read_mask = 0, zero_mask = 0, patched_mask = 0;
    uint8_t saved_mask = 0, restore_attempted_mask = 0;
    uint8_t restored_mask = 0, restore_skipped_mask = 0;
    uint64_t context_generation = 0, write_generation = 0, restore_generation = 0;
    uint64_t last_address = 0, error_address = 0;
    float original_values[6]{};
    float last_before = 0.f, last_after = 0.f, error_value = 0.f;
    int error_stage = 0;
};
inline bool valid(float f) {
    return viewmodel::finite(f) && f >= -1000.f && f <= 1000.f;
}

template<class IO> class Controller {
    IO& io;
    uint64_t session_ = 0, base_ = 0;
    Context context_{};
    // Profile gravity/air drag/underwater drag, then the same fields on prefab.
    std::array<Scalar, 6> saved_{};
    uint64_t probe_player_ = 0, probe_fp_ = 0, probe_weapon_ = 0;
    uint64_t probe_config_ = 0, probe_profile_ = 0, probe_projectile_ = 0;
    Kind probe_kind_ = Kind::Unknown;
    uint64_t context_generation_ = 0, write_generation_ = 0, restore_generation_ = 0;
    uint8_t read_mask_ = 0, zero_mask_ = 0, patched_mask_ = 0;
    uint8_t restore_attempted_mask_ = 0, restored_mask_ = 0, restore_skipped_mask_ = 0;
    uint64_t last_address_ = 0, error_address_ = 0;
    float last_before_ = 0.f, last_after_ = 0.f, error_value_ = 0.f;
    int error_stage_ = 0;

    const Identity& owner(size_t i) const {
        return i < 3 ? context_.profile : context_.projectile;
    }
    bool target_available(size_t i) const {
        return owner(i).object >= 0x10000;
    }
    uint64_t address(size_t i) const {
        using namespace go_active;
        static const uint64_t profile_offsets[3] = {
            BALLISTICPROFILE_GRAVITY,
            BALLISTICPROFILE_AIR_DRAG,
            BALLISTICPROFILE_UNDERWATER_DRAG,
        };
        static const uint64_t projectile_offsets[3] = {
            SHAFTEDPROJECTILE_GRAVITY,
            SHAFTEDPROJECTILE_AIR_DRAG,
            SHAFTEDPROJECTILE_UNDERWATER_DRAG,
        };
        return owner(i).object + (i < 3 ? profile_offsets[i] : projectile_offsets[i - 3]);
    }
    bool fail(int stage, uint64_t address = 0, float value = 0.f) {
        stage_ = stage;
        error_stage_ = stage;
        error_address_ = address;
        error_value_ = value;
        return false;
    }
    void clear_error() {
        error_stage_ = 0;
        error_address_ = 0;
        error_value_ = 0.f;
    }
    bool restore() {
        bool ok = true;
        restore_attempted_mask_ = restored_mask_ = restore_skipped_mask_ = 0;
        for (size_t i = 0; i < saved_.size(); ++i) {
            auto& s = saved_[i];
            if (!s.owned) continue;
            const uint8_t bit = static_cast<uint8_t>(1u << i);
            const uint64_t a = address(i);
            restore_attempted_mask_ |= bit;
            const Life life = owner(i).life(io);
            if (life == Life::Unreadable) { fail(18, a); ok = false; continue; }
            if (life == Life::Gone) { restore_skipped_mask_ |= bit; s = {}; continue; }
            float now = 0.f;
            if (!io.read(a, now)) { fail(18, a); ok = false; continue; }
            if (!valid(now)) { restore_skipped_mask_ |= bit; s = {}; continue; }
            // Do not overwrite another writer's update; restore only our zero.
            if (now == 0.f && s.original != 0.f) {
                last_address_ = a;
                last_before_ = now;
                last_after_ = s.original;
                if (!io.write_heap(a, s.original)) { fail(18, a, now); ok = false; continue; }
                float after = 0.f;
                if (!io.read(a, after) || after != s.original) {
                    fail(18, a, after); ok = false; continue;
                }
                last_after_ = after;
                restored_mask_ |= bit;
                ++restore_generation_;
            } else if (now != 0.f) {
                restore_skipped_mask_ |= bit;
            }
            s = {};
        }
        return ok;
    }

    int stage_ = 0;

    bool resolve(uint64_t player, Context& c) {
        using namespace go_active;
        c = {};
        probe_player_ = player;
        probe_fp_ = probe_weapon_ = probe_config_ = 0;
        probe_profile_ = probe_projectile_ = 0;
        probe_kind_ = Kind::Unknown;
        uint64_t net = 0, fp = 0, weapon = 0, config = 0;
        uint64_t profile = 0, projectile = 0, back = 0;
        uint8_t local = 0;
        if (!player || !io.named(player, "PlayerManager", "Oxide")) return fail(1, player);
        if (!c.player.capture(io, player, true)) return fail(2, player);
        if (!pointer(io, player + NETWORK_BEHAVIOUR_IDENTITY, net) ||
            !io.named(net, "NetworkIdentity", "Mirror"))
            return fail(3, player + NETWORK_BEHAVIOUR_IDENTITY);
        if (!io.read(net + NETWORK_IDENTITY_IS_LOCAL_PLAYER, local) || local != 1)
            return fail(4, net + NETWORK_IDENTITY_IS_LOCAL_PLAYER, static_cast<float>(local));
        if (!pointer(io, player + PLAYER_FP_MANAGER, fp))
            return fail(5, player + PLAYER_FP_MANAGER);
        probe_fp_ = fp;
        if (!io.named(fp, "FPManager", "Oxide") || !c.fp.capture(io, fp, true))
            return fail(5, fp);

        // Prefer the actual equipped object; use the typed cache only if absent.
        uint64_t active = 0;
        const bool active_read = pointer(io, fp + FPMANAGER_CURRENT_OBJECT, active);
        if (active_read && active) weapon = active;
        else if (!pointer(io, fp + FPMANAGER_CURRENT_WEAPON, weapon))
            return fail(6, fp + FPMANAGER_CURRENT_WEAPON);
        probe_weapon_ = weapon;
        if (!weapon) return fail(6, fp + FPMANAGER_CURRENT_OBJECT);
        c.kind = fast_fire::classify(io, weapon);
        probe_kind_ = c.kind;
        if (c.kind == Kind::Unknown) return fail(6, weapon);
        if (!pointer(io, weapon + FPOBJECT_PLAYER_BACKREF, back) || back != player)
            return fail(7, weapon + FPOBJECT_PLAYER_BACKREF);
        if (!c.weapon.capture(io, weapon, true)) return fail(7, weapon);

        if (has_profile(c.kind)) {
            const uint64_t field = config_offset(c.kind);
            if (!field || !pointer(io, weapon + field, config) || !config)
                return fail(8, weapon + field);
            probe_config_ = config;
            if (!io.named(config, "HitscanWeaponConfig", kWeapons) ||
                !c.config.capture(io, config, true)) return fail(8, config);
            if (!pointer(io, config + HITSCANCONFIG_BALLISTIC_PROFILE, profile))
                return fail(9, config + HITSCANCONFIG_BALLISTIC_PROFILE);
            probe_profile_ = profile;
            if (profile) {
                if (!io.named(profile, "BallisticProfile", kBallistics) ||
                    !c.profile.capture(io, profile, false)) return fail(9, profile);
            } else if (c.kind == Kind::Hitscan) {
                return fail(9, config + HITSCANCONFIG_BALLISTIC_PROFILE);
            }
        }

        if (has_projectile(c.kind)) {
            const uint64_t field = projectile_offset(c.kind);
            if (!field || !pointer(io, weapon + field, projectile))
                return fail(10, weapon + field);
            probe_projectile_ = projectile;
            if (projectile) {
                if (!io.named(projectile, "ShaftedProjectile", "Oxide") ||
                    !c.projectile.capture(io, projectile, true)) return fail(11, projectile);
            } else if (!has_profile(c.kind)) {
                return fail(10, weapon + field);
            }
        }
        // Melee/tools are recognized too; they simply have no ballistic data.
        if (!c.profile.object && !c.projectile.object && c.kind != Kind::Melee)
            return fail(12, weapon);
        stage_ = 0;
        return true;
    }

    bool apply(uint64_t player) {
        read_mask_ = zero_mask_ = patched_mask_ = 0;
        for (size_t i = 0; i < saved_.size(); ++i) {
            if (!target_available(i)) continue;
            const uint8_t bit = static_cast<uint8_t>(1u << i);
            const uint64_t a = address(i);
            float now = 0.f;
            if (!io.read(a, now)) return fail(13, a);
            read_mask_ |= bit;
            if (!valid(now)) return fail(13, a, now);
            if (now == 0.f) { zero_mask_ |= bit; continue; }
            Context fresh;
            if (!resolve(player, fresh)) return false;
            if (!(fresh == context_)) return fail(14, a, now);
            if (!io.read(a, now)) return fail(15, a);
            read_mask_ |= bit;
            if (!valid(now)) return fail(15, a, now);
            if (now == 0.f) { zero_mask_ |= bit; continue; }
            auto& s = saved_[i];
            s.original = now;
            s.owned = true;
            last_address_ = a;
            last_before_ = now;
            last_after_ = 0.f;
            if (!io.write_heap(a, 0.f)) return fail(16, a, now);
            if (!io.read(a, now) || now != 0.f) return fail(17, a, now);
            last_after_ = now;
            patched_mask_ |= bit;
            ++write_generation_;
        }
        stage_ = 0;
        clear_error();
        return true;
    }

public:
    viewmodel::Status status = viewmodel::Status::Off;
    explicit Controller(IO& memory) : io(memory) {}
    int stage() const { return stage_; }
    bool has_state() const {
        for (const auto& s : saved_) if (s.owned) return true;
        return false;
    }
    DebugInfo debug_info() const {
        DebugInfo d;
        d.kind = context_.kind;
        d.probe_kind = probe_kind_;
        d.weapon = context_.weapon.object;
        d.config = context_.config.object;
        d.profile = context_.profile.object;
        d.projectile = context_.projectile.object;
        d.probe_player = probe_player_;
        d.probe_fp = probe_fp_;
        d.probe_weapon = probe_weapon_;
        d.probe_config = probe_config_;
        d.probe_profile = probe_profile_;
        d.probe_projectile = probe_projectile_;
        d.read_mask = read_mask_;
        d.zero_mask = zero_mask_;
        d.patched_mask = patched_mask_;
        d.restore_attempted_mask = restore_attempted_mask_;
        d.restored_mask = restored_mask_;
        d.restore_skipped_mask = restore_skipped_mask_;
        d.context_generation = context_generation_;
        d.write_generation = write_generation_;
        d.restore_generation = restore_generation_;
        d.last_address = last_address_;
        d.last_before = last_before_;
        d.last_after = last_after_;
        d.error_stage = error_stage_;
        d.error_address = error_address_;
        d.error_value = error_value_;
        for (size_t i = 0; i < saved_.size(); ++i) {
            const uint8_t bit = static_cast<uint8_t>(1u << i);
            if (target_available(i)) d.target_mask |= bit;
            if (saved_[i].owned) d.saved_mask |= bit;
            d.original_values[i] = saved_[i].original;
        }
        return d;
    }
    bool reset(bool restore_originals) {
        const bool had_context = context_.weapon.object || context_.profile.object || context_.projectile.object;
        const bool ok = !restore_originals || restore();
        saved_ = {};
        context_ = {};
        if (had_context) ++context_generation_;
        session_ = base_ = 0;
        stage_ = 0;
        status = viewmodel::Status::Off;
        if (ok) clear_error();
        if (!restore_originals) {
            read_mask_ = zero_mask_ = patched_mask_ = 0;
            restore_attempted_mask_ = restored_mask_ = restore_skipped_mask_ = 0;
            probe_player_ = probe_fp_ = probe_weapon_ = 0;
            probe_config_ = probe_profile_ = probe_projectile_ = 0;
            probe_kind_ = Kind::Unknown;
            clear_error();
        }
        return ok;
    }
    void tick(uint64_t session, uint64_t base, uint64_t player, bool enabled) {
        using viewmodel::Status;
        if (session != session_ || base != base_) {
            reset(false);
            session_ = session;
            base_ = base;
        }
        status = enabled ? Status::Waiting : Status::Off;
        if (!enabled) {
            stage_ = 0;
            if (!restore()) status = Status::Failed;
            else clear_error();
            return;
        }
        if (!base) return;
        Context next;
        if (!resolve(player, next)) {
            if (!restore()) status = Status::Failed;
            return;
        }
        if (!(next == context_)) {
            if (!restore()) { status = Status::Failed; return; }
            context_ = next;
            ++context_generation_;
        }
        status = apply(player) ? Status::Active : Status::Failed;
    }
};
}

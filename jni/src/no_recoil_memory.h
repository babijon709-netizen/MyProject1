#pragma once
// «Без отдачи» для хитскан-стволов.
//
// Откуда берётся отдача (проверено по дампам 11946 и 11943): выстрел зовёт
// WeaponRecoil из FPHitscan (+0x200), а тот черпает амплитуду из ассета
// RecoilConfig, который висит на HitscanWeaponConfig ствола (+0x70, рядом с
// разбросом +0x68). Ассет один на модель ствола и живёт в куче — значит
// достаточно погасить в нём амплитуду: вертикальные Мин/Макс (0x18/0x20,
// Vector2) и боковые дельты (0x28/0x2C). Код игры не трогается; при
// выключении/смене ствола/отвалке процесса оригиналы возвращаются — тот же
// приём, что у «нет разброса» (см. weapon_tuning_memory.h).
//
// Только хитскан: у луков/арбалетов/ближнего своих RecoilConfig в цепочке
// нет, и для них контроллер честно стоит в ожидании, а не пишет наугад.
#include "viewmodel.h"
#include "game_offsets_active.h"
#include "weapon_tuning_memory.h"   // pointer(), Identity, Life, kWeapons
#include <array>

namespace no_recoil {
using weapon_tuning::pointer;
using weapon_tuning::Identity;
using weapon_tuning::Life;
using weapon_tuning::kWeapons;

struct Context {
    Identity player, fp, weapon, config, recoil;
    bool operator==(const Context& b) const {
        return player==b.player && fp==b.fp && weapon==b.weapon &&
            config==b.config && recoil==b.recoil;
    }
};
struct Scalar {
    float original = 0.f;
    bool owned = false;
};
inline bool valid(float f) { return viewmodel::finite(f) && f >= -1000.f && f <= 1000.f; }

template<class IO> class Controller {
    IO& io;
    uint64_t session_ = 0, base_ = 0;
    Context context_{};
    // minX, minY, maxX, maxY, minSide, maxSide — всё в ассете отдачи.
    std::array<Scalar,6> saved_{};
    uint64_t address(size_t i) const {
        using namespace go_active;
        static const uint64_t offs[6] = {
            RECOILCONFIG_MIN_RECOIL, RECOILCONFIG_MIN_RECOIL + 4,
            RECOILCONFIG_MAX_RECOIL, RECOILCONFIG_MAX_RECOIL + 4,
            RECOILCONFIG_MIN_SIDE,   RECOILCONFIG_MAX_SIDE,
        };
        return context_.recoil.object + offs[i];
    }
    bool restore() {
        bool ok = true;
        const Life state = context_.recoil.object ? context_.recoil.life(io) : Life::Gone;
        for (size_t i = 0; i < saved_.size(); ++i) {
            auto& s = saved_[i];
            if (!s.owned) continue;
            if (state == Life::Unreadable) { ok = false; continue; }
            if (state == Life::Gone) { s = {}; continue; }
            float now = 0;
            if (!io.read(address(i), now)) { ok = false; continue; }
            if (!viewmodel::finite(now)) { s = {}; continue; }
            // Не затираем чужое обновление: возвращаем, только если там всё
            // ещё наш ноль.
            if (now == 0.f && s.original != 0.f) {
                if (!io.write_heap(address(i), s.original)) { ok = false; continue; }
                if (!io.read(address(i), now) || now != s.original) { ok = false; continue; }
            }
            s = {};
        }
        return ok;
    }
    int stage_ = 0;
    bool fail(int stage) { stage_ = stage; return false; }

    bool resolve(uint64_t player, Context& c) {
        using namespace go_active;
        uint64_t net=0, fp=0, weapon=0, config=0, recoil=0, back=0;
        uint8_t local=0;
        if (!player || !io.named(player,"PlayerManager","Oxide")) return fail(1);
        if (!c.player.capture(io,player,true)) return fail(2);
        if (!pointer(io,player+NETWORK_BEHAVIOUR_IDENTITY,net) ||
            !io.named(net,"NetworkIdentity","Mirror")) return fail(3);
        if (!io.read(net+NETWORK_IDENTITY_IS_LOCAL_PLAYER,local) || local!=1) return fail(4);
        if (!pointer(io,player+PLAYER_FP_MANAGER,fp) ||
            !io.named(fp,"FPManager","Oxide") || !c.fp.capture(io,fp,true)) return fail(5);
        // Тот же выбор ствола, что у разброса/темпа: авторитетный +0x50,
        // типизированный кэш +0x58 — только запасной.
        uint64_t active=0;
        const bool active_read=pointer(io,fp+FPMANAGER_CURRENT_OBJECT,active);
        if (active_read && active) weapon=active;
        else if (!pointer(io,fp+FPMANAGER_CURRENT_WEAPON,weapon)) return fail(6);
        if (!weapon || !io.named(weapon,"FPHitscan","Oxide")) return fail(6);
        if (!pointer(io,weapon+FPOBJECT_PLAYER_BACKREF,back) || back!=player) return fail(7);
        if (!c.weapon.capture(io,weapon,true)) return fail(7);
        if (!pointer(io,weapon+FPHITSCAN_CONFIG,config) ||
            !io.named(config,"HitscanWeaponConfig",kWeapons) ||
            !c.config.capture(io,config,true)) return fail(8);
        if (!pointer(io,config+HITSCANCONFIG_RECOIL_CONFIG,recoil) ||
            !io.named(recoil,"RecoilConfig",kWeapons) ||
            !c.recoil.capture(io,recoil,true)) return fail(9);
        stage_ = 0;
        return true;
    }
    bool apply(uint64_t player) {
        for (size_t i = 0; i < saved_.size(); ++i) {
            float now = 0;
            if (!io.read(address(i), now) || !valid(now)) return fail(10);
            if (now == 0.f) continue;
            Context fresh;
            // Перед каждой записью заново подтверждаем всю цепочку.
            if (!resolve(player, fresh)) return false;
            if (!(fresh == context_)) return fail(11);
            if (!io.read(address(i), now) || !valid(now)) return fail(12);
            if (now == 0.f) continue;
            auto& s = saved_[i];
            s.original = now; s.owned = true;
            if (!io.write_heap(address(i), 0.f)) return fail(13);
            if (!io.read(address(i), now) || now != 0.f) return fail(14);
        }
        return true;
    }
public:
    viewmodel::Status status = viewmodel::Status::Off;
    explicit Controller(IO& memory) : io(memory) {}
    int stage() const { return stage_; }
    bool has_state() const { for (const auto& s : saved_) if (s.owned) return true; return false; }
    bool reset(bool restore_originals) {
        const bool ok = !restore_originals || restore();
        saved_ = {}; context_ = {}; session_ = base_ = 0;
        status = viewmodel::Status::Off;
        return ok;
    }
    void tick(uint64_t session, uint64_t base, uint64_t player, bool enabled) {
        using viewmodel::Status;
        if (session != session_ || base != base_) {
            reset(false); session_ = session; base_ = base;
        }
        status = enabled ? Status::Waiting : Status::Off;
        if (!enabled && !restore()) status = Status::Failed;
        if (!enabled || !base) return;
        Context next;
        if (!resolve(player, next)) {
            if (!restore()) status = Status::Failed;
            return;
        }
        if (!(next == context_)) {
            // Сменился ствол/ассет: старый возвращаем, новый перехватываем.
            if (!restore()) saved_ = {};
            context_ = next;
        }
        status = apply(player) ? Status::Active : Status::Failed;
    }
};
}

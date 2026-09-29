#pragma once
// Object-data implementation: no instruction writes, hooks, or library-data
// writes. IO exposes read, named(object,name,namespace), and write_heap only.
#include "viewmodel.h"
#include "game_offsets_active.h"
#include <array>

namespace weapon_tuning {
inline constexpr const char* kWeapons = "HyperHug.Games.Oxide.Features.Weapons";
template<class IO> bool pointer(IO& io, uint64_t a, uint64_t& value) {
    if (!io.read(a, value)) return false;
    value &= 0x00ffffffffffffffULL;
    return true;
}
enum class Life { Alive, Gone, Unreadable };
struct Identity {
    uint64_t object = 0, klass = 0, native = 0;
    bool unity = false;
    template<class IO> bool capture(IO& io, uint64_t obj, bool is_unity) {
        object = obj; unity = is_unity;
        return obj >= 0x10000 && pointer(io,obj,klass) && klass >= 0x10000 &&
            (!unity || (pointer(io,obj+go_active::MANAGED_CACHED_PTR,native) && native >= 0x10000));
    }
    template<class IO> Life life(IO& io) const {
        uint64_t k=0, n=0;
        if (!pointer(io,object,k)) return Life::Unreadable;
        if (k != klass) return Life::Gone;
        if (unity) {
            if (!pointer(io,object+go_active::MANAGED_CACHED_PTR,n)) return Life::Unreadable;
            if (n != native) return Life::Gone;
        }
        return Life::Alive;
    }
    bool operator==(const Identity& b) const {
        return object==b.object && klass==b.klass && native==b.native && unity==b.unity;
    }
};
struct Context {
    Identity player, fp, weapon, config, recoil, dispersion, calculator;
    bool operator==(const Context& b) const {
        return player==b.player && fp==b.fp && weapon==b.weapon && config==b.config &&
            recoil==b.recoil && dispersion==b.dispersion && calculator==b.calculator;
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
    // Recoil: six consecutive floats beginning at MinRecoil (two vectors and
    // side deltas). Spread: named min/max plus the calculator's cached base.
    std::array<Scalar,9> saved_{};
    const Identity& owner(size_t i) const {
        return i<6 ? context_.recoil : i<8 ? context_.dispersion : context_.calculator;
    }
    uint64_t address(size_t i) const {
        if (i < 6) return owner(i).object + go_active::RECOILCONFIG_MIN_RECOIL + i * sizeof(float);
        if (i == 6) return owner(i).object + go_active::DISPERSIONCONFIG_MIN_DISPERSION;
        if (i == 7) return owner(i).object + go_active::DISPERSIONCONFIG_MAX_DISPERSION;
        return owner(i).object + go_active::CALCULATOR_CACHED_BASE_DISPERSION;
    }
    Life calculator_life() {
        auto life = context_.weapon.life(io);
        if (life != Life::Alive) return life;
        uint64_t calc=0, config=0;
        if (!pointer(io,context_.weapon.object+go_active::FPHITSCAN_CALCULATOR,calc) ||
            !pointer(io,context_.calculator.object+go_active::CALCULATOR_HITSCAN_CONFIG,config))
            return Life::Unreadable;
        if (calc != context_.calculator.object || config != context_.config.object) return Life::Gone;
        return context_.calculator.life(io);
    }
    bool restore(size_t begin, size_t end) {
        bool ok = true;
        for (size_t i=begin; i<end; ++i) {
            auto& s = saved_[i];
            if (!s.owned) continue;
            Life life = i==8 ? calculator_life() : owner(i).life(io);
            if (life == Life::Unreadable) { ok=false; continue; }
            if (life == Life::Gone) { s={}; continue; }
            float now=0;
            if (!io.read(address(i),now)) { ok=false; continue; }
            if (!viewmodel::finite(now)) { s={}; continue; }
            // Never overwrite another writer's update or a destroyed object.
            if (now == 0.f && s.original != 0.f) {
                if (!io.write_heap(address(i),s.original)) { ok=false; continue; }
                if (!io.read(address(i),now) || now != s.original) { ok=false; continue; }
            }
            s={};
        }
        return ok;
    }
    // Где именно оборвалась цепочка. Без этого отказ выглядит одинаково
    // («не применено»), а причин у него полтора десятка: от «оружие в руках
    // не огнестрел» до «сменилось имя класса калькулятора».
    int stage_ = 0;
    bool fail(int stage) { stage_ = stage; return false; }

    bool resolve(uint64_t player, Context& c) {
        uint64_t net=0, fp=0, weapon=0, config=0, recoil=0, dispersion=0, calc=0, back=0, p=0;
        uint8_t local=0;
        if (!player || !io.named(player,"PlayerManager","Oxide")) return fail(1);
        if (!c.player.capture(io,player,true)) return fail(2);
        if (!pointer(io,player+go_active::NETWORK_BEHAVIOUR_IDENTITY,net) ||
            !io.named(net,"NetworkIdentity","Mirror")) return fail(3);
        if (!io.read(net+go_active::NETWORK_IDENTITY_IS_LOCAL_PLAYER,local) || local!=1)
            return fail(4);
        if (!pointer(io,player+go_active::PLAYER_FP_MANAGER,fp) ||
            !io.named(fp,"FPManager","Oxide") || !c.fp.capture(io,fp,true)) return fail(5);

        auto accept_weapon = [&](uint64_t candidate) {
            uint64_t owner=0;
            return candidate && io.named(candidate,"FPHitscan","Oxide") &&
                pointer(io,candidate+go_active::FPOBJECT_PLAYER_BACKREF,owner) &&
                owner==player && (weapon=candidate,true);
        };
        // _currentWeapon (+0x50) is authoritative. A non-null melee/tool must
        // block the stale typed FPWeaponBase cache at +0x58; only an absent or
        // null authoritative slot may use that compatibility fallback.
        uint64_t active=0;
        const bool active_read=pointer(io,fp+go_active::FPMANAGER_CURRENT_OBJECT,active);
        if (active_read && active) {
            if (!accept_weapon(active)) return fail(6);
        } else {
            uint64_t typed=0;
            if (!pointer(io,fp+go_active::FPMANAGER_CURRENT_WEAPON,typed) ||
                !accept_weapon(typed)) return fail(6);
        }

        if (!c.weapon.capture(io,weapon,true) ||
            !pointer(io,weapon+go_active::FPOBJECT_PLAYER_BACKREF,back) || back!=player)
            return fail(7);
        if (!pointer(io,weapon+go_active::FPHITSCAN_CONFIG,config) ||
            !io.named(config,"HitscanWeaponConfig",kWeapons) ||
            !c.config.capture(io,config,true)) return fail(8);
        if (!pointer(io,config+go_active::HITSCANCONFIG_RECOIL_CONFIG,recoil) || !recoil)
            return fail(9);
        // Embedded WeaponRecoil supplies an independent backlink when present.
        // Some builds initialize it a frame later, so an unreadable/null value
        // is not used as the primary source.
        if (pointer(io,weapon+go_active::FPHITSCAN_RECOIL+
                       go_active::WEAPONRECOIL_RECOIL_CONFIG,p) && p && p!=recoil)
            return fail(10);
        if (!io.named(recoil,"RecoilConfig",kWeapons) || !c.recoil.capture(io,recoil,true))
            return fail(11);
        if (!pointer(io,weapon+go_active::FPHITSCAN_CALCULATOR,calc) || !calc) return fail(12);
        if (!io.calculator_class(calc) || !c.calculator.capture(io,calc,false)) return fail(13);
        if (!pointer(io,calc+go_active::CALCULATOR_HITSCAN_CONFIG,p) || p!=config) return fail(14);
        if (!pointer(io,calc+go_active::CALCULATOR_DISPERSION_CONFIG,dispersion) ||
            !pointer(io,config+go_active::HITSCANCONFIG_DISPERSION_CONFIG,p) || p!=dispersion)
            return fail(15);
        if (!io.named(dispersion,"DispersionConfig",kWeapons) ||
            !c.dispersion.capture(io,dispersion,true)) return fail(16);
        stage_ = 0;
        return true;
    }
    bool apply(size_t begin, size_t end, uint64_t player) {
        for (size_t i=begin; i<end; ++i) {
            float now=0;
            if (!io.read(address(i),now) || !valid(now)) return fail(17);
            if (now==0.f) continue; // no repeated writes to already-zero data
            Context fresh;
            // Revalidate the local owner, equip links and asset identities
            // immediately before each write, not only at frame start.
            if (!resolve(player,fresh)) return false;          // stage_ уже выставлен
            if (!(fresh==context_)) return fail(18);
            if (!io.read(address(i),now) || !valid(now)) return fail(19);
            if (now==0.f) continue;
            auto& s=saved_[i];
            s.original=now; s.owned=true; // retain original even on failed write/readback
            if (!io.write_heap(address(i),0.f)) return fail(20);   // ворота записи
            if (!io.read(address(i),now) || now!=0.f) return fail(21); // запись не легла
        }
        return true;
    }
public:
    viewmodel::Status recoil = viewmodel::Status::Off, spread = viewmodel::Status::Off;
    explicit Controller(IO& memory) : io(memory) {}
    // 0 — всё разрешилось; иначе номер шага из resolve()/apply(), см. fail().
    int stage() const { return stage_; }
    bool has_state() const { for (const auto& s:saved_) if (s.owned) return true; return false; }
    bool reset(bool restore_originals) {
        bool ok=!restore_originals || restore(0,saved_.size());
        // Callers disconnect after reset. Never carry addresses into another pid.
        saved_={}; context_={}; session_=base_=0;
        recoil=spread=viewmodel::Status::Off;
        return ok;
    }
    void tick(uint64_t session, uint64_t base, uint64_t player, bool nr, bool ns) {
        using viewmodel::Status;
        if (session!=session_ || base!=base_) {
            reset(false); session_=session; base_=base;
        }
        recoil=nr?Status::Waiting:Status::Off;
        spread=ns?Status::Waiting:Status::Off;
        // Restore even without a local player: ScriptableObject assets may
        // outlive respawn/unequip. Cached calculator requires old weapon links.
        bool r=nr || restore(0,6), s=ns || restore(6,9);
        if (!r) recoil=Status::Failed;
        if (!s) spread=Status::Failed;
        if (!r || !s || (!nr && !ns)) return;
        if (!base) return;
        // No offset discovery and no native-RVA table: every address below is
        // `object + go_active::<named offset>` from the release header. Live
        // reads only validate object ownership.
        Context next;
        if (!resolve(player,next)) {
            if (!restore(0,9)) recoil=spread=Status::Failed;
            return;
        }
        if (!(next==context_)) {
            if (!restore(0,9)) { recoil=spread=Status::Failed; return; }
            context_=next;
        }
        if (nr) recoil=apply(0,6,player)?Status::Active:Status::Failed;
        if (ns) spread=apply(6,9,player)?Status::Active:Status::Failed;
    }
};
}

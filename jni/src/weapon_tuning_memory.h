#pragma once
// Object-data implementation: no instruction writes, hooks, or library-data
// writes. IO exposes read, named(object,name,namespace), and write_heap only.
#include "viewmodel.h"
#include "game_offsets_active.h"
#include <array>

namespace weapon_tuning {
struct Layout {
    std::array<uint8_t,20> build_id;
    const char* calculator;
    // Read-only checks reject a process still carrying the retired patches.
    std::array<uint64_t,4> guard_sites;
    std::array<uint32_t,4> guard_words;
};
inline constexpr Layout kLayouts[] = {
    {{0xd8,0x5e,0x63,0xcd,0x8d,0xcb,0xd4,0x81,0x69,0x50,0x12,0xef,0x2e,0x28,0x4f,0x0e,0xeb,0x9e,0xf3,0x07},
     "Ze", {0x660c214,0x73425c8,0x73425f0,0x5b5cad4}, {0xfd000120,0x1e214c40,0x1e210800,0xbd495100}},
    {{0xe1,0x07,0x95,0x34,0x9b,0xd7,0xc2,0xcd,0xc7,0xd8,0x52,0x75,0x97,0xc6,0x43,0x95,0xa4,0xef,0xed,0x79},
     "FJ", {0x6565aec,0x72dae70,0x72db044,0x5af4d04}, {0xfd000120,0x1e214c40,0x1e210800,0xbd400100}},
    // Новая бета: четыре инструкции из native WeaponRecoil, dynamic/base
    // dispersion. Имя calculator ротируется: проверяем его namespace и обе
    // обратные ссылки на те же строго типизированные конфиги.
    {{0x76,0xfb,0xd4,0xff,0xa6,0x61,0xfe,0x71,0x17,0xee,0x41,0x72,0xbd,0xdc,0xf7,0xd6,0x9a,0xfe,0x15,0xa0},
     nullptr, {0x7422c80,0x7422c9c,0x741d978,0x741d8e8},
     {0xbd401d00,0x2d450500,0x2d440502,0xbd402001}},
};
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
            (!unity || (pointer(io,obj+0x10,native) && native >= 0x10000));
    }
    template<class IO> Life life(IO& io) const {
        uint64_t k=0, n=0;
        if (!pointer(io,object,k)) return Life::Unreadable;
        if (k != klass) return Life::Gone;
        if (unity) {
            if (!pointer(io,object+0x10,n)) return Life::Unreadable;
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
    int resolve_step_ = 0; // 1 identity, 2 fp/weapon, 3 config, 4 recoil, 5 calculator/dispersion
    // recoil: config+18..2c (six floats). Spread: clamp min/max +20/+24,
    // then calculator's per-instance cached base +20. No damage/rate writes.
    std::array<Scalar,9> saved_{};
    const Identity& owner(size_t i) const {
        return i<6 ? context_.recoil : i<8 ? context_.dispersion : context_.calculator;
    }
    uint64_t address(size_t i) const {
        return owner(i).object + (i<6 ? 0x18+i*4 : i<8 ? 0x20+(i-6)*4 : 0x20);
    }
    Life calculator_life() {
        auto life = context_.weapon.life(io);
        if (life != Life::Alive) return life;
        uint64_t calc=0, config=0;
        if (!pointer(io,context_.weapon.object+0x1f8,calc) ||
            !pointer(io,context_.calculator.object+0x18,config)) return Life::Unreadable;
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
    bool resolve(uint64_t player, const Layout& layout, Context& c) {
        uint64_t fp=0, weapon=0, config=0, recoil=0, dispersion=0, calc=0, back=0, p=0;
        uint8_t local=0;
        resolve_step_ = 1;
        if (!player || !io.named(player,"PlayerManager","Oxide") ||
            !c.player.capture(io,player,true) || !pointer(io,player+0x40,p) ||
            !io.named(p,"NetworkIdentity","Mirror") || !io.read(p+0x4a,local) || local!=1)
            return false;
        resolve_step_ = 2;
        if (!pointer(io,player+go_active::PLAYER_FP_MANAGER,fp) || !io.named(fp,"FPManager","Oxide") ||
            !c.fp.capture(io,fp,true) || !pointer(io,fp+go_active::FPMANAGER_CURRENT_WEAPON,weapon) ||
            !weapon || !io.named(weapon,"FPHitscan","Oxide") || !c.weapon.capture(io,weapon,true) ||
            !pointer(io,weapon+go_active::FPOBJECT_PLAYER_BACKREF,back) || back!=player)
            return false;
        resolve_step_ = 3;
        if (!pointer(io,weapon+0x130,config) || !io.named(config,"HitscanWeaponConfig",kWeapons) ||
            !c.config.capture(io,config,true) || !pointer(io,weapon+0x200,recoil) ||
            !pointer(io,config+0x68,p) || p!=recoil || !pointer(io,weapon+0x208,p) || p!=config)
            return false;
        resolve_step_ = 4;
        if (!io.named(recoil,"RecoilConfig",kWeapons) || !c.recoil.capture(io,recoil,true) ||
            !pointer(io,weapon+0x1f8,calc) ||
            !(layout.calculator ? io.named(calc,layout.calculator,"") : io.calculator_class(calc)) ||
            !c.calculator.capture(io,calc,false)) return false;
        resolve_step_ = 5;
        if (!pointer(io,calc+0x18,p) || p!=config ||
            !pointer(io,calc+0x10,dispersion) || !pointer(io,config+0x60,p) || p!=dispersion ||
            !io.named(dispersion,"DispersionConfig",kWeapons) ||
            !c.dispersion.capture(io,dispersion,true)) return false;
        resolve_step_ = 0;
        return true;
    }
    bool apply(size_t begin, size_t end, uint64_t player, const Layout& layout) {
        for (size_t i=begin; i<end; ++i) {
            float now=0;
            if (!io.read(address(i),now) || !valid(now)) return false;
            if (now==0.f) continue; // no repeated writes to already-zero data
            Context fresh;
            // Revalidate the local owner, equip links and asset identities
            // immediately before each write, not only at frame start.
            if (!resolve(player,layout,fresh) || !(fresh==context_)) return false;
            if (!io.read(address(i),now) || !valid(now)) return false;
            if (now==0.f) continue;
            auto& s=saved_[i];
            s.original=now; s.owned=true; // retain original even on failed write/readback
            if (!io.write_heap(address(i),0.f)) return false;
            if (!io.read(address(i),now) || now!=0.f) return false;
        }
        return true;
    }
public:
    // 0 ready/off, 1 unknown ELF build-id, 2 guard instruction,
    // 10..14 owner-chain stage,
    // 20 restoration, 21 heap write/readback. Exposed for device diagnostics.
    int diagnostic = 0;
    int guard_index = -1;
    uint32_t guard_observed = 0;
    std::array<uint8_t,20> observed_id{};
    viewmodel::Status recoil = viewmodel::Status::Off, spread = viewmodel::Status::Off;
    explicit Controller(IO& memory) : io(memory) {}
    bool has_state() const { for (const auto& s:saved_) if (s.owned) return true; return false; }
    bool reset(bool restore_originals) {
        bool ok=!restore_originals || restore(0,saved_.size());
        // Callers disconnect after reset. Never carry addresses into another pid.
        saved_={}; context_={}; session_=base_=0;
        diagnostic=0; guard_index=-1; guard_observed=0; observed_id={};
        recoil=spread=viewmodel::Status::Off;
        return ok;
    }
    void tick(uint64_t session, uint64_t base, uint64_t player, bool nr, bool ns) {
        using viewmodel::Status;
        if (session!=session_ || base!=base_) {
            reset(false); session_=session; base_=base;
        }
        diagnostic=0; guard_index=-1; guard_observed=0;
        recoil=nr?Status::Waiting:Status::Off;
        spread=ns?Status::Waiting:Status::Off;
        // Restore even without a local player: ScriptableObject assets may
        // outlive respawn/unequip. Cached calculator requires old weapon links.
        bool r=nr || restore(0,6), s=ns || restore(6,9);
        if (!r) recoil=Status::Failed;
        if (!s) spread=Status::Failed;
        if (!r || !s) diagnostic=20;
        if (!r || !s || (!nr && !ns)) return;
        std::array<uint8_t,20> id{};
        const Layout* layout=nullptr;
        if (base && io.read(base+0x318,id))
            for (const auto& l:kLayouts) if (l.build_id==id) layout=&l;
        observed_id=id;
        bool supported=layout!=nullptr;
        if (!layout) diagnostic=1;
        if (layout) for (size_t i=0;i<layout->guard_sites.size();++i) {
            uint32_t word=0;
            if (!io.read(base+layout->guard_sites[i],word) || word!=layout->guard_words[i]) {
                supported=false; diagnostic=2; guard_index=(int)i; guard_observed=word;
                break;
            }
        }
        if (!supported) {
            restore(0,9);
            if (nr) recoil=Status::Failed;
            if (ns) spread=Status::Failed;
            return;
        }
        Context next;
        if (!resolve(player,*layout,next)) {
            diagnostic=9+resolve_step_;
            if (!restore(0,9)) { recoil=spread=Status::Failed; diagnostic=20; }
            return;
        }
        if (!(next==context_)) {
            if (!restore(0,9)) { recoil=spread=Status::Failed; diagnostic=20; return; }
            context_=next;
        }
        if (nr && (recoil=apply(0,6,player,*layout)?Status::Active:Status::Failed)==Status::Failed)
            diagnostic=resolve_step_ ? 9+resolve_step_ : 21;
        if (ns && (spread=apply(6,9,player,*layout)?Status::Active:Status::Failed)==Status::Failed)
            diagnostic=resolve_step_ ? 9+resolve_step_ : 21;
    }
};
}

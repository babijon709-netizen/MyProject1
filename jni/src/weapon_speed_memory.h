#pragma once
// Быстрая стрельба и перезарядка для уже созданного FPHitscan.
//
// Одних полей HitscanWeaponConfig недостаточно: FPHitscan.lMf/sPo копирует
// интервалы в живой FPWeaponBase/FPHitscan ещё при создании/доставании оружия.
// Поэтому контроллер синхронно держит и config, и подтверждённые ARM64-аудитом
// live-кэши. Инструкции/данные библиотек не меняются, а все адресные цепочки
// состоят только из статических go_active offsets текущего релиза.
//
// Жизненный цикл (почему тут столько кода про смерть игрока):
//
// 1. Конфиг — это общий ScriptableObject, он переживает смерть. Пока тумблер
//    включён, в нём лежит НАШЕ ускоренное значение. Новое оружие после
//    респавна собирается именно из него, поэтому его live-кэши с рождения
//    ускорены. Принять такую копию за «оригинал» нельзя: следующее ускорение
//    поделит её ещё раз, и после пары смертей период выстрела вырождается в
//    ноль — оружие просто перестаёт стрелять, пока тумблер не перещёлкнут.
//    Отсюда clean_ (эталон оригиналов на конфиг) и Scalar::unpoison_*.
// 2. Владелец (оружие, WaitForSeconds) может исчезнуть ровно в тот момент,
//    когда мы возвращаем ему оригинал: чтение адреса срывается, restore()
//    честно сообщает «не смог». Раньше такой отказ намертво держал
//    контроллер на СТАРОМ контексте — новое оружие не принималось, обе
//    функции навсегда уходили в Failed и лечились только выключением и
//    включением тумблера. Теперь незакрытое восстановление уезжает в
//    очередь pending_ и повторяется в фоне, а живой контекст принимается
//    сразу.
// 3. Стрельба и перезарядка независимы: отказ восстановления у выключенной
//    функции больше не мешает применению включённой.
#include "viewmodel.h"
#include "game_offsets_active.h"
#include <cstddef>
#include <cstdint>

namespace weapon_speed {
inline constexpr const char* kWeapons = "HyperHug.Games.Oxide.Features.Weapons";
// Переключатели всегда используют прежний максимум UI; промежуточных
// коэффициентов больше нет.
inline constexpr float kMaxMultiplier = 20.f;
// Нижние пределы записываемых интервалов. Вырожденный (нулевой/денормальный)
// период — это не «очень быстро», а сломанная стрельба: пара тактов игрового
// цикла с нулём, и оружие не стреляет вовсе.
inline constexpr float kMinShotPeriod = 0.0005f;
inline constexpr float kMinBurstSeconds = 0.0005f;

template<class IO> bool pointer(IO& io, uint64_t address, uint64_t& value) {
    if (!io.read(address,value)) return false;
    value &= 0x00ffffffffffffffULL;
    return true;
}

enum class Life { Alive, Gone, Unreadable };

struct Identity {
    uint64_t object=0, klass=0, native=0;
    bool unity=false;

    template<class IO> bool capture(IO& io,uint64_t obj,bool is_unity) {
        object=obj; unity=is_unity;
        return obj>=0x10000 && pointer(io,obj,klass) && klass>=0x10000 &&
            (!unity || (pointer(io,obj+go_active::MANAGED_CACHED_PTR,native) && native>=0x10000));
    }
    template<class IO> Life life(IO& io) const {
        uint64_t k=0,n=0;
        if (!pointer(io,object,k)) return Life::Unreadable;
        if (k!=klass) return Life::Gone;
        if (unity) {
            if (!pointer(io,object+go_active::MANAGED_CACHED_PTR,n)) return Life::Unreadable;
            if (n!=native) return Life::Gone;
        }
        return Life::Alive;
    }
    bool operator==(const Identity& other) const {
        return object==other.object && klass==other.klass && native==other.native && unity==other.unity;
    }
};

struct Context {
    Identity player,fp,weapon,config,burst_wait;
    bool operator==(const Context& other) const {
        return player==other.player && fp==other.fp && weapon==other.weapon &&
            config==other.config && burst_wait==other.burst_wait;
    }
};

inline bool finite_range(float value,float lo,float hi) {
    return viewmodel::finite(value) && value>=lo && value<=hi;
}

struct Scalar {
    uint64_t address=0;
    float original=0.f,last=0.f,current=0.f;
    bool owned=false;
    // Оригинал захвачен именно этим sample() — только такой можно править
    // как «копию нашего ускорения» (см. unpoison_*).
    bool fresh=false;

    template<class IO> bool sample(IO& io,uint64_t addr,float lo,float hi) {
        float value=0.f;
        fresh=false;
        if (!io.read(addr,value) || !finite_range(value,lo,hi)) return false;
        // If the game rebuilt a cache while the toggle was active, adopt that
        // new unowned value instead of restoring an obsolete lifecycle value.
        if (!owned || address!=addr || value!=last) { original=value; fresh=true; }
        address=addr; current=value;
        return true;
    }
    // Значение ассета (HitscanWeaponConfig). Статические данные релиза не
    // меняются сами по себе: если свежий «оригинал» отличается от эталона
    // кратно (это наше же ускорение, оставшееся в конфиге с прошлой жизни
    // игрока), берём эталон как есть.
    void unpoison_asset(float clean) {
        if (!fresh || !(clean>0.f) || !(original>0.f)) return;
        if (original<=clean*0.25f || original>=clean*4.f) original=clean;
    }
    // Живой кэш оружия: игра пересчитывает его из конфига и множителей
    // статов, поэтому точного эталона нет. Копия нашего ускорения опознаётся
    // по кратности (в 4+ раза быстрее известного оригинала) и разворачивается
    // тем же множителем — так переживает и смену статов.
    void unpoison_copy(float clean,float multiplier,float hi) {
        if (!fresh || !(clean>0.f) || !(original>0.f) || !(multiplier>1.f)) return;
        if (original>clean*0.25f) return;
        float restored=original*multiplier;
        if (!viewmodel::finite(restored)) return;
        if (restored>hi) restored=hi;
        original=restored;
    }
    template<class IO> bool put(IO& io,float value) {
        if (!address || !viewmodel::finite(value)) return false;
        if (current==value) { last=value; owned=true; return true; }
        // Preserve ownership even on failure: reset can retry restoration of
        // any earlier writes from this same frame.
        owned=true;
        if (!io.write_heap(address,value)) return false;
        current=last=value;
        float check=0.f;
        if (!io.read(address,check) || check!=value) return false;
        return true;
    }
    template<class IO> bool normalize_copy(IO& io,uint64_t addr) const {
        if (!owned || !addr) return true;
        float value=0.f;
        if (!io.read(addr,value) || !viewmodel::finite(value)) return false;
        // A newly constructed weapon can copy our accelerated config/cache
        // before the next frame.  Only undo an exact copy of our last value;
        // an already-existing holstered weapon keeps its independent original.
        if (value==last && original!=last) {
            if (!io.write_heap(addr,original)) return false;
            if (!io.read(addr,value) || value!=original) return false;
        }
        return true;
    }
    template<class IO> bool restore(IO& io,const Identity& owner) {
        if (!owned) { *this={}; return true; }
        const Life life=owner.life(io);
        if (life==Life::Unreadable) return false;
        if (life==Life::Gone) { *this={}; return true; }
        float value=0.f;
        if (!io.read(address,value)) return false;
        if (!viewmodel::finite(value)) { *this={}; return true; }
        // Conditional restore: never overwrite a value subsequently changed
        // by the game or another lifecycle owner.
        if (value==last && original!=last) {
            if (!io.write_heap(address,original)) return false;
            if (!io.read(address,value) || value!=original) return false;
        }
        *this={};
        return true;
    }
};

template<class IO> class Controller {
    IO& io_;
    uint64_t session_=0,base_=0;
    Context context_{};

    // Config source values.
    Scalar config_fire_{},config_full_{},config_loop_{};
    // Values copied by FPHitscan.lMf/sPo into an already-live weapon.
    Scalar shot_period_{},effective_full_{},effective_loop_{},burst_seconds_{};

    // Эталон оригиналов для текущего конфига: по нему опознаются копии нашего
    // же ускорения у заново созданного оружия (респавн, смена оружия).
    struct Clean {
        Identity config{};
        float fire=0.f,config_full=0.f,config_loop=0.f;
        float shot=0.f,full=0.f,loop=0.f,burst=0.f;
    };
    Clean clean_{};

    // Сколько кадров подряд (около секунды) цепочка не разрешается, прежде
    // чем снимать ускорение. Одиночный промах — это сбой чтения (на
    // устройстве отказывают единицы процентов чтений), и откатываться по
    // нему нельзя: получалось «то работает, то нет», а каждый второй кадр
    // уходил на лишние записи. Настоящая смерть держится куда дольше.
    static constexpr int kResolveMissLimit=60;
    int resolve_miss_=0;

    // Отложенное восстановление: владелец исчез или не читается прямо сейчас.
    struct Pending {
        Identity owner{};
        uint64_t address=0;
        float original=0.f,last=0.f;
        int attempts=0;
    };
    // ~5 секунд при 60 кадрах: после этого объект уже точно не вернётся.
    static constexpr int kPendingAttempts=300;
    static constexpr std::size_t kPendingSlots=8;
    Pending pending_[kPendingSlots]{};
    std::size_t pending_count_=0;

    void defer(const Identity& owner,const Scalar& s) {
        if (!s.owned || !s.address) return;
        for (std::size_t i=0;i<pending_count_;++i)
            if (pending_[i].address==s.address) {
                pending_[i]=Pending{owner,s.address,s.original,s.last,0};
                return;
            }
        if (pending_count_>=kPendingSlots) return;   // очередь полна: забываем
        pending_[pending_count_++]=Pending{owner,s.address,s.original,s.last,0};
    }
    // Снять ускорение и освободить слот. true — оригинал уже возвращён (или
    // объект честно мёртв); false — восстановление отложено в очередь.
    bool retire(Scalar& s,const Identity& owner) {
        if (!s.owned) { s={}; return true; }
        const Scalar copy=s;
        if (s.restore(io_,owner)) return true;       // restore сам обнулил s
        defer(owner,copy);
        s={};
        return false;
    }
    void drain_pending() {
        std::size_t out=0;
        for (std::size_t i=0;i<pending_count_;++i) {
            Pending p=pending_[i];
            Scalar s;
            s.address=p.address; s.original=p.original; s.last=p.last; s.owned=true;
            if (s.restore(io_,p.owner)) continue;    // вернули или объект мёртв
            if (++p.attempts>=kPendingAttempts) continue;
            pending_[out++]=p;
        }
        pending_count_=out;
    }

    bool restore_fire() {
        const bool a=retire(config_fire_,context_.config);
        const bool b=retire(shot_period_,context_.weapon);
        const bool c=retire(burst_seconds_,context_.burst_wait);
        return a && b && c;
    }
    bool restore_reload() {
        const bool a=retire(config_full_,context_.config);
        const bool b=retire(config_loop_,context_.config);
        const bool c=retire(effective_full_,context_.weapon);
        const bool d=retire(effective_loop_,context_.weapon);
        return a && b && c && d;
    }
    bool restore_all() {
        const bool a=restore_fire();
        const bool b=restore_reload();
        return a && b;
    }
    // Уборка унаследованного ускорения на ЖИВОМ оружии, которое мы не
    // трогали: оно собрано игрой из ускоренного конфига. Копия опознаётся по
    // кратности относительно эталона (как в Scalar::unpoison_copy) и
    // разворачивается тем же множителем. Без этого выключенный тумблер
    // оставлял только что полученный после респавна ствол ускоренным до
    // ближайшего перезахода в руки.
    void unwind_copy(uint64_t address,float clean,float multiplier,float hi) {
        if (!address || !(clean>0.f) || !(multiplier>1.f)) return;
        float value=0.f;
        if (!io_.read(address,value) || !viewmodel::finite(value) || value<=0.f) return;
        if (value>clean*0.25f) return;               // не наша копия
        float restored=value*multiplier;
        if (!viewmodel::finite(restored) || restored>hi) return;
        if (!io_.write_heap(address,restored)) return;
        float check=0.f;
        (void)(io_.read(address,check) && check==restored);
    }
    void sweep_inherited(const Context& live,bool fire_fields,bool reload_fields) {
        // Эталон принадлежит конкретному конфигу; чужое оружие не трогаем.
        if (!clean_.config.object || !(clean_.config==live.config)) return;
        if (live.weapon==context_.weapon && owned_state()) return; // это наше, его закроет restore
        if (fire_fields) {
            unwind_copy(live.weapon.object+go_active::FPWEAPON_SHOT_PERIOD,
                        clean_.shot,kMaxMultiplier,120.f);
            if (live.burst_wait.object)
                unwind_copy(live.burst_wait.object+go_active::WAITFORSECONDS_SECONDS,
                            clean_.burst,kMaxMultiplier,120.f);
        }
        if (reload_fields) {
            unwind_copy(live.weapon.object+go_active::FPWEAPON_EFFECTIVE_RELOAD_PERIOD,
                        clean_.full,kMaxMultiplier,120.f);
            unwind_copy(live.weapon.object+go_active::FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD,
                        clean_.loop,kMaxMultiplier,120.f);
        }
    }
    void normalize_inherited_caches(const Context& next) {
        if (!(context_.config==next.config) || context_.weapon==next.weapon) return;
        shot_period_.normalize_copy(
            io_,next.weapon.object+go_active::FPWEAPON_SHOT_PERIOD);
        effective_full_.normalize_copy(
            io_,next.weapon.object+go_active::FPWEAPON_EFFECTIVE_RELOAD_PERIOD);
        effective_loop_.normalize_copy(
            io_,next.weapon.object+go_active::FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD);
    }

    bool resolve(uint64_t player,Context& c) {
        uint64_t net=0,fp=0,weapon=0,back=0,config=0;
        uint8_t local=0;
        if (!player || !io_.named(player,"PlayerManager","Oxide") ||
            !c.player.capture(io_,player,true) ||
            !pointer(io_,player+go_active::NETWORK_BEHAVIOUR_IDENTITY,net) ||
            !io_.named(net,"NetworkIdentity","Mirror") ||
            !io_.read(net+go_active::NETWORK_IDENTITY_IS_LOCAL_PLAYER,local) || local!=1 ||
            !pointer(io_,player+go_active::PLAYER_FP_MANAGER,fp) ||
            !io_.named(fp,"FPManager","Oxide") || !c.fp.capture(io_,fp,true))
            return false;

        auto accept_weapon = [&](uint64_t candidate) {
            uint64_t owner=0;
            return candidate && io_.named(candidate,"FPHitscan","Oxide") &&
                pointer(io_,candidate+go_active::FPOBJECT_PLAYER_BACKREF,owner) &&
                owner==player && (weapon=candidate,true);
        };

        // _currentWeapon (+0x50) is the authoritative active FPObject.  A
        // valid but stale typed cache at +0x58 must not win after switching.
        // Fall back to +0x58 only while the authoritative slot is absent/null;
        // if +0x50 contains a melee/tool, do not patch the previously held gun.
        uint64_t active=0;
        const bool active_read=pointer(io_,fp+go_active::FPMANAGER_CURRENT_OBJECT,active);
        if (active_read && active) {
            if (!accept_weapon(active)) return false;
        } else {
            uint64_t typed=0;
            if (!pointer(io_,fp+go_active::FPMANAGER_CURRENT_WEAPON,typed) ||
                !accept_weapon(typed)) return false;
        }

        if (!c.weapon.capture(io_,weapon,true) ||
            !pointer(io_,weapon+go_active::FPOBJECT_PLAYER_BACKREF,back) || back!=player ||
            !pointer(io_,weapon+go_active::FPHITSCAN_CONFIG,config) ||
            !io_.named(config,"HitscanWeaponConfig",kWeapons) ||
            !c.config.capture(io_,config,true)) return false;

        // Burst cadence is cached in a managed WaitForSeconds.  It is optional
        // for guards/status, but when the exact object and a sane duration are
        // present it is owned/restored like every other live cache.
        uint64_t wait=0;
        float seconds=0.f;
        if (pointer(io_,weapon+go_active::FPHITSCAN_BURST_WAIT,wait) && wait &&
            io_.named(wait,"WaitForSeconds","UnityEngine") &&
            io_.read(wait+go_active::WAITFORSECONDS_SECONDS,seconds) &&
            finite_range(seconds,0.000001f,120.f)) {
            if (!c.burst_wait.capture(io_,wait,false)) return false;
        }
        return true;
    }

    bool apply_fire(float multiplier) {
        if (!config_fire_.sample(io_,context_.config.object+go_active::HITSCANCONFIG_FIRE_RATE,
                                0.0001f,10000.f)) return false;
        config_fire_.unpoison_asset(clean_.fire);
        if (!shot_period_.sample(io_,context_.weapon.object+go_active::FPWEAPON_SHOT_PERIOD,
                                 0.000001f,120.f)) return false;
        shot_period_.unpoison_copy(clean_.shot,multiplier,120.f);
        if (context_.burst_wait.object) {
            if (!burst_seconds_.sample(io_,context_.burst_wait.object+go_active::WAITFORSECONDS_SECONDS,
                                       0.000001f,120.f)) return false;
            burst_seconds_.unpoison_copy(clean_.burst,multiplier,120.f);
        }
        // Эталон на текущий конфиг: по нему опознаётся оружие, рождённое из
        // уже ускоренного конфига (респавн, смена оружия того же типа).
        if (config_fire_.original>0.f) clean_.fire=config_fire_.original;
        if (shot_period_.original>0.f) clean_.shot=shot_period_.original;
        if (context_.burst_wait.object && burst_seconds_.original>0.f)
            clean_.burst=burst_seconds_.original;

        float wanted=config_fire_.original>=1.f
            ? config_fire_.original*multiplier : config_fire_.original/multiplier;
        if (wanted<0.01f) wanted=0.01f;
        if (wanted>5000.f) wanted=5000.f;
        float shot=shot_period_.original/multiplier;
        if (shot<kMinShotPeriod) shot=kMinShotPeriod;
        float burst=burst_seconds_.original/multiplier;
        if (burst<kMinBurstSeconds) burst=kMinBurstSeconds;

        const bool a=config_fire_.put(io_,wanted);
        const bool b=shot_period_.put(io_,shot);
        const bool c=!context_.burst_wait.object || burst_seconds_.put(io_,burst);
        return a && b && c;
    }

    bool apply_reload(float multiplier) {
        if (!config_full_.sample(io_,context_.config.object+go_active::HITSCANCONFIG_FULL_RELOAD,
                                0.f,120.f)) return false;
        config_full_.unpoison_asset(clean_.config_full);
        if (!config_loop_.sample(io_,context_.config.object+go_active::HITSCANCONFIG_LOOP_RELOAD,
                                0.f,120.f)) return false;
        config_loop_.unpoison_asset(clean_.config_loop);
        // Do not take a transient zero as the "original" of a cache whose
        // config mode is enabled; sPo may initialize it later from our already
        // accelerated config, which would otherwise be accelerated a 2nd time.
        const float full_min=config_full_.original>0.f ? 0.000001f : 0.f;
        const float loop_min=config_loop_.original>0.f ? 0.000001f : 0.f;
        if (!effective_full_.sample(io_,context_.weapon.object+go_active::FPWEAPON_EFFECTIVE_RELOAD_PERIOD,
                                    full_min,120.f)) return false;
        effective_full_.unpoison_copy(clean_.full,multiplier,120.f);
        if (!effective_loop_.sample(io_,context_.weapon.object+go_active::FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD,
                                    loop_min,120.f)) return false;
        effective_loop_.unpoison_copy(clean_.loop,multiplier,120.f);
        if (config_full_.original>0.f) clean_.config_full=config_full_.original;
        if (config_loop_.original>0.f) clean_.config_loop=config_loop_.original;
        if (effective_full_.original>0.f) clean_.full=effective_full_.original;
        if (effective_loop_.original>0.f) clean_.loop=effective_loop_.original;

        float full=config_full_.original>0.f ? config_full_.original/multiplier : 0.f;
        float loop=config_loop_.original>0.f ? config_loop_.original/multiplier : 0.f;
        if (full>0.f && full<0.03f) full=0.03f;
        if (loop>0.f && loop<0.01f) loop=0.01f;

        // The full period may include a player stat multiplier; scale the
        // captured effective value by the same config ratio instead of
        // replacing it with the raw config. Loop period is copied 1:1 today,
        // but ratio scaling also remains correct if that changes.
        const float full_scale=config_full_.original>0.f
            ? full/config_full_.original : 1.f/multiplier;
        const float loop_scale=config_loop_.original>0.f
            ? loop/config_loop_.original : 1.f/multiplier;
        const float effective_full=effective_full_.original>0.f
            ? effective_full_.original*full_scale : 0.f;
        const float effective_loop=effective_loop_.original>0.f
            ? effective_loop_.original*loop_scale : 0.f;

        const bool a=config_full_.put(io_,full);
        const bool b=config_loop_.put(io_,loop);
        const bool c=effective_full_.put(io_,effective_full);
        const bool d=effective_loop_.put(io_,effective_loop);
        return a && b && c && d;
    }

public:
    viewmodel::Status fire=viewmodel::Status::Off,reload=viewmodel::Status::Off;
    explicit Controller(IO& io):io_(io){}

    // Ускорение, которое прямо сейчас числится за нами (без фоновых долгов).
    bool owned_state() const {
        return config_fire_.owned || config_full_.owned || config_loop_.owned ||
            shot_period_.owned || effective_full_.owned || effective_loop_.owned ||
            burst_seconds_.owned;
    }
    bool has_state() const { return pending_count_!=0 || owned_state(); }
    bool reset(bool restore) {
        bool ok=true;
        if (restore) {
            drain_pending();
            ok=restore_all();
            // Единственная попытка добить то, что только что отложилось:
            // дальше адреса уносить некуда — сессия закрывается.
            drain_pending();
            ok=ok && pending_count_==0;
        }
        context_={};
        config_fire_={}; config_full_={}; config_loop_={};
        shot_period_={}; effective_full_={}; effective_loop_={}; burst_seconds_={};
        clean_={};
        pending_count_=0;
        resolve_miss_=0;
        session_=base_=0;
        fire=reload=viewmodel::Status::Off;
        return ok;
    }
    void tick(uint64_t session,uint64_t base,uint64_t player,bool fast_fire,bool fast_reload) {
        using viewmodel::Status;
        if (session!=session_ || base!=base_) { reset(false); session_=session; base_=base; }
        fire=fast_fire?Status::Waiting:Status::Off;
        reload=fast_reload?Status::Waiting:Status::Off;
        // Долги прошлых жизней: вернуть оригиналы объектам, которые в момент
        // отката не читались. Ни на что дальше не влияет.
        drain_pending();

        if (!fast_fire && !fast_reload) {
            // Обе выключены. Пока состояние ещё наше — один проход по живому
            // оружию: оно могло родиться (респавн, смена ствола) из уже
            // ускоренного конфига и унаследовать наши значения.
            if (base && owned_state()) {
                Context live;
                if (resolve(player,live)) sweep_inherited(live,true,true);
            }
            if (!restore_fire()) fire=Status::Failed;
            if (!restore_reload()) reload=Status::Failed;
            return;
        }
        if (!base) return;

        // No offset discovery: live reads only validate exact local objects
        // selected through release go_active constants.
        Context next;
        if (!resolve(player,next)) {
            // Игрок мёртв, оружие убрано или просто сорвалось чтение. Первое
            // отличается от второго только устойчивостью: откатываемся лишь
            // после серии промахов, иначе функция мигает на каждом сбое.
            if (++resolve_miss_>=kResolveMissLimit) restore_all();
            return;
        }
        resolve_miss_=0;
        if (!(next==context_)) {
            // Живой контекст принимается ВСЕГДА. Незакрытые откаты старого
            // оружия — забота pending_, а не повод отключить функцию.
            normalize_inherited_caches(next);
            restore_all();
            context_=next;
        }
        if (!(clean_.config==next.config)) { clean_={}; clean_.config=next.config; }
        // Выключенная половина: сначала снять унаследованное ускорение с
        // живого оружия, потом вернуть оригиналы своим значениям. Функции
        // независимы — отказ отката одной не отменяет применение другой.
        if (!fast_fire || !fast_reload) sweep_inherited(next,!fast_fire,!fast_reload);
        if (!fast_fire && !restore_fire()) fire=Status::Failed;
        if (!fast_reload && !restore_reload()) reload=Status::Failed;
        if (fast_fire) fire=apply_fire(kMaxMultiplier)?Status::Active:Status::Failed;
        if (fast_reload) reload=apply_reload(kMaxMultiplier)?Status::Active:Status::Failed;
    }
};
}

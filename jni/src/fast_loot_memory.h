#pragma once
// Быстрый подбор — «Забрать все» без межкадровых пауз.
//
// Как это устроено в игре (проверено по дампам 11946/11943): экран лута
// собирает контейнер асинхронным циклом <CollectAllItems>d__* и между
// предметами ждёт UniTask.Delay(0.35f) — MoveNext этих стейт-машин грузит
// 0.35f из пула литералов .rodata и зовёт обёртку UniTask.Delay, а
// SlotTransferAnimationConfig держит ту же 0.35 как длительность перелёта
// иконки. Пока идёт сбор, в плеер-лупе UniTask всегда висит активный
// DelayPromise с delayTimeSpan == 0.35f.
//
// Что делает модуль: с периодом 16 мс (8 мс сразу после обнуления — пока
// идёт сбор, следующий промис появляется в том же кадре) обходит
// PlayerLoopHelper.static_fields.runners (массив PlayerLoopRunner) →
// loopItems[0..tail) и у каждого промиса с точным размером 0.35f обнуляет
// delayTimeSpan. UniTask.DelayPromise.MoveNext завершает промис, как только
// elapsed >= delayTimeSpan, поэтому обнуление дотягивает его в следующем же
// кадре — весь «Забрать все» схлопывается до пары кадров. Код игры не
// трогается, пишется только одно поле в живых объектах кучи. Чтения
// пакетные (массив раннеров, пара tail+items, массив предметов — по одному
// запросу на диапазон), чтобы частый проход не упирался в транспорт.
//
// Безопасность записи: классы промисов резолвятся ОДИН раз за сессию из
// TypeInfo-слотов сборки и подтверждаются именем («DelayPromise» /
// «DelayIgnoreTimeScalePromise», вложенные классы — пустой неймспейс);
// дальше элемент принимается только точным совпадением указателя класса.
// Перед записью проверяется, что само поле ещё держит 0.35f, а elapsed —
// конечное и разумное. Другие размеры задержек не трогаются вовсе.
//
// Память — только через IO (read_quiet/запись выбранным транспортом,
// см. HeapMemory в game_viewmodel.cpp): своего /proc/<pid>/mem у модуля нет
// (KERNEL.md §1).

#include "game_offsets_active.h"
#include "weapon_tuning_memory.h"   // weapon_tuning::pointer (чтение указателя без TBI-метки)
#include <chrono>
#include <cstdint>

namespace fast_loot {

using Clock = std::chrono::steady_clock;

// Каденция «Забрать все». Только точное совпадение: другие потребители
// 0.35f в игре есть (анимации тех-дерева, питакеты), но их промисы сюда
// попадают лишь пока активны, а точный фильтр — та же граница, что была у
// прежнего обхода промисов.
inline constexpr float kCadence = 0.35f;
inline constexpr int kMaxRunners = 64;         // PlayerLoopTiming значений ~24, берём с запасом
inline constexpr int kMaxItemsPerRunner = 512; // живой плеер-луп обычно в пределах сотни
// Период прохода: 16 мс в покое и 8 мс сразу после обнуления — пока идёт
// «Забрать все», игра спавнит следующий промис в том же кадре, где дотянут
// предыдущий, и горячий период подхватывает его почти без паузы на предмет.
inline constexpr auto kPeriod = std::chrono::milliseconds(16);
inline constexpr auto kHotPeriod = std::chrono::milliseconds(8);
inline constexpr auto kResolveRetry = std::chrono::seconds(2);

// TBI/MTE-метка в старшем байте указателей из памяти игры (см. mem_io.h).
inline std::uint64_t untag(std::uint64_t p) { return p & 0x00ffffffffffffffULL; }

enum class State { Off, Waiting, Active, Failed };

// Диагностика «иногда не работает»: каждая точка, где проход может молча
// ничего не найти, складывает сюда счётчик. Наружу отдаётся снимком через
// diag(), репорты в лог строит вызывающая сторона (game_viewmodel.cpp) —
// у заголовка своего логгера нет и не будет.
struct Diag {
    Clock::time_point enabled_at{};   // reset при включении фичи
    int  resolve_attempts = 0;        // попыток резолва слотов/классов
    int  static_lost = 0;             // runners под helper_static_ не читается (статика переехала)
    int  sweeps = 0;                  // проходов дошло до обхода элементов
    int  abort_mask = 0;              // биты стадий обрыва прохода (см. sweep)
    int  items_seen = 0;              // элементов раннеров просмотрено
    int  klass_read_fails = 0;        // класс элемента не прочитался
    int  klass_hits = 0;              // элемент — DelayPromise/IgnoreTimeScalePromise
    int  cadence_misses = 0;          // класс тот, timespan != 0.35f (всего)
    int  cadence_near = 0;            // из них около-луковые (0.05, 1.6]с — см. zero_if_cadence
    int  aborts = 0;                  // обрывов прохода суммарно (для оконной дельты)
    int  elapsed_rejects = 0;         // отброшено проверкой elapsed
    int  write_fails = 0;             // write_heap отказал (ворота логируют сами)
    float seen_timespans[4] = {};     // до 4 разных около-луковых timespan у «своих» классов
    int   seen_timespan_hits[4] = {};
    std::uint64_t other_klass[3] = {}; // первые 3 класса элементов НЕ из списка промисов
};

template<class IO> class Controller {
public:
    explicit Controller(IO& io) : io_(io) {}

    void reset() {
        helper_static_ = 0;
        klass_scaled_ = klass_unscaled_ = 0;
        state_ = State::Waiting;
        next_resolve_ = {};
        zeroed_total_ = 0;
        last_batch_ = 0;
        stage_ = 0;
        diag_ = {};   // указатели прежней сессии в диагностике бессмысленны
    }

    State state() const { return state_; }
    int stage() const { return stage_; }           // где не сложился резолв (см. resolve)
    int zeroed_total() const { return zeroed_total_; }
    int last_batch() const { return last_batch_; } // обнулено за последний тик
    const Diag& diag() const { return diag_; }
    void diag_reset() { diag_ = {}; diag_.enabled_at = Clock::now(); }

    // Возвращает число обнулённых задержек (0 — ничего не попалось).
    int tick(uint64_t il2cpp_base, bool enabled) {
        if (!enabled) { state_ = State::Off; return 0; }
        const auto now = Clock::now();
        if (now - last_tick_ < (last_batch_ > 0 ? kHotPeriod : kPeriod)) return 0;
        last_tick_ = now;
        if (!il2cpp_base) return 0;

        if ((helper_static_ == 0 || (klass_scaled_ == 0 && klass_unscaled_ == 0)) &&
            now >= next_resolve_) {
            next_resolve_ = now + kResolveRetry;
            resolve(il2cpp_base);
        }
        if (helper_static_ == 0 || (klass_scaled_ == 0 && klass_unscaled_ == 0)) return 0;

        const int zeroed = sweep();
        last_batch_ = zeroed;
        zeroed_total_ += zeroed;
        state_ = State::Active;
        return zeroed;
    }

private:
    // stage: 1 — слот PlayerLoopHelper не подтвердился именем,
    //        2 — статические поля класса не прочитались,
    //        3 — ни один класс промисов не подтвердился.
    bool fail(int stage) { stage_ = stage; state_ = State::Failed; return false; }

    uint64_t resolve_promise_class(uint64_t base, uint64_t rva, uint64_t alt, const char* name) {
        const uint64_t slots[2] = {rva, alt};
        for (uint64_t slot : slots) {
            if (!slot) continue;
            uint64_t klass = 0;
            if (!weapon_tuning::pointer(io_, base + slot, klass)) continue;
            // Вложенные классы в il2cpp живут с пустым неймспейсом.
            // Имя не прочиталось (прецедент EIO в метаданных) — верим слоту:
            // он из таблицы этой сборки. Прочиталось — только точное имя.
            if (klass >= 0x10000 && io_.named_klass(klass, name, "")) return klass;
        }
        return 0;
    }

    void resolve(uint64_t base) {
        stage_ = 0;
        ++diag_.resolve_attempts;
        uint64_t helper = 0;
        if (!weapon_tuning::pointer(io_, base + go_active::PLAYER_LOOP_HELPER_TYPEINFO_RVA, helper) ||
            helper < 0x10000 || !io_.named_klass(helper, "PlayerLoopHelper", "Cysharp.Threading.Tasks")) {
            fail(1);
            return;
        }
        uint64_t sf = 0;
        if (!weapon_tuning::pointer(io_, helper + go_active::IL2CPP_CLASS_STATIC_FIELDS_OFFSET, sf) ||
            sf < 0x10000) {
            fail(2);
            return;
        }
        klass_scaled_ = resolve_promise_class(
            base, go_active::FASTLOOT_DELAYPROMISE_TYPEINFO_RVA,
            go_active::FASTLOOT_DELAYPROMISE_TYPEINFO_RVA_ALT, "DelayPromise");
        klass_unscaled_ = resolve_promise_class(
            base, go_active::FASTLOOT_IGNOREDELAY_TYPEINFO_RVA,
            go_active::FASTLOOT_IGNOREDELAY_TYPEINFO_RVA_ALT, "DelayIgnoreTimeScalePromise");
        if (!klass_scaled_ && !klass_unscaled_) { fail(3); return; }
        helper_static_ = sf;
        state_ = State::Active;
    }

    // Около-луковое окно каденции. Фон игры (виден в benzware.log: промисы
    // ровно на 1.00/2.00/9.70/10.00 с тикают постоянно, сотнями за 10 с) —
    // это посторонние DelayPromise: лечиться как «лутовая пауза» они не
    // могут и только замусоривают слоты и репорты. Лутовая пауза живёт
    // ниже секунды; если в новой сборке её изменят, искать надо значение,
    // которое ПОЯВИЛОСЬ в этом окне позже базовой линии.
    static bool near_cadence(float v) { return v > 0.05f && v <= 1.60f; }

    // Запоминает значение timespan «своего» класса, отличное от каденции:
    // главный признак, что в этом билде пауза «Забрать все» другая.
    void note_timespan(float value) {
        for (int i = 0; i < 4; ++i) {
            if (diag_.seen_timespan_hits[i] > 0 && diag_.seen_timespans[i] == value) {
                ++diag_.seen_timespan_hits[i]; return;
            }
            if (diag_.seen_timespan_hits[i] == 0) {
                diag_.seen_timespans[i] = value; diag_.seen_timespan_hits[i] = 1; return;
            }
        }
    }

    // Первые три чужих класса, встреченных в плеер-лупе: если DelayPromise
    // не находится вообще, их имена подскажут, что вместо него.
    void note_other_klass(uint64_t klass) {
        for (int i = 0; i < 3; ++i) {
            if (diag_.other_klass[i] == klass) return;
            if (diag_.other_klass[i] == 0) { diag_.other_klass[i] = klass; return; }
        }
    }

    // Обнуляет одну задержку, если поле всё ещё держит точную каденцию.
    int zero_if_cadence(uint64_t item, uint64_t timespan_off) {
        float timespan = 0.f, elapsed = 0.f;
        if (!io_.read(item + timespan_off, timespan) || timespan != kCadence) {
            if (viewmodel::finite(timespan) && timespan > 0.001f && timespan <= 30.f) {
                ++diag_.cadence_misses;
                if (near_cadence(timespan)) { ++diag_.cadence_near; note_timespan(timespan); }
            }
            return 0;
        }
        // elapsed живёт следом за полем размера в обеих раскладках: защита от
        // мусорного «попадания» в переработанный объект кучи.
        if (!io_.read(item + timespan_off + 4, elapsed)) return 0;
        if (!viewmodel::finite(elapsed) || elapsed < 0.f || elapsed > 4.f * kCadence) {
            ++diag_.elapsed_rejects;
            return 0;
        }
        const float zero = 0.f;
        if (!io_.write_heap(item + timespan_off, zero)) { ++diag_.write_fails; return 0; }
        return 1;
    }

    // tail (0x30) и loopItems (0x38) лежат одним куском — читаются разом.
    struct RunnerHead { std::int32_t tail; std::uint32_t pad; std::uint64_t items; };
    static_assert(sizeof(RunnerHead) == 16, "tail+items pair layout");

    // abort_mask: 1 — указатель runners не читается, 2 — длина массива
    // раннеров неправдоподобна, 4 — массив раннеров не прочитался,
    // 8 — шапка раннера (tail+items), 16 — capacity элементов,
    // 32 — массив элементов. Биты липкие до diag_reset: проход отказывается
    // то в одном месте, то в другом, и одного «последнего» номера мало.
    int sweep() {
        uint64_t runners = 0;
        if (!weapon_tuning::pointer(io_, helper_static_ + go_active::PLAYERLOOPHELPER_RUNNERS, runners) ||
            runners < 0x10000) {
            helper_static_ = 0; // статика переехала (перезапуск сцены) — резолв заново
            ++diag_.static_lost;
            ++diag_.aborts;
            diag_.abort_mask |= 1;
            return 0;
        }
        int32_t runner_count = 0;
        if (!io_.read(runners + go_active::IL2CPP_ARRAY_LENGTH, runner_count) ||
            runner_count <= 0 || runner_count > kMaxRunners) {
            ++diag_.aborts;
            diag_.abort_mask |= 2;
            return 0;
        }

        std::uint64_t runner_ptrs[kMaxRunners] = {};
        if (!io_.read_range(runners + go_active::IL2CPP_ARRAY_FIRST_ELEMENT,
                            runner_ptrs, (std::size_t)runner_count * sizeof(std::uint64_t))) {
            ++diag_.aborts;
            diag_.abort_mask |= 4;
            return 0;
        }
        ++diag_.sweeps;

        std::uint64_t items_buf[kMaxItemsPerRunner];
        int zeroed = 0;
        for (int32_t r = 0; r < runner_count; ++r) {
            const uint64_t runner = untag(runner_ptrs[r]);
            if (runner < 0x10000) continue;
            RunnerHead head{};
            if (!io_.read(runner + go_active::LOOPRUNNER_TAIL, head)) {
                ++diag_.aborts;
                diag_.abort_mask |= 8; continue;
            }
            if (head.tail <= 0) continue;
            const uint64_t items = untag(head.items);
            if (items < 0x10000) continue;
            int32_t capacity = 0;
            if (!io_.read(items + go_active::IL2CPP_ARRAY_LENGTH, capacity) || capacity <= 0) {
                ++diag_.aborts;
                diag_.abort_mask |= 16; continue;
            }
            const int32_t count = head.tail < capacity ? head.tail : capacity;
            const int32_t limit = count < kMaxItemsPerRunner ? count : kMaxItemsPerRunner;
            // Указатели элементов — одним чтением на весь диапазон.
            if (!io_.read_range(items + go_active::IL2CPP_ARRAY_FIRST_ELEMENT,
                                items_buf, (std::size_t)limit * sizeof(std::uint64_t))) {
                ++diag_.aborts;
                diag_.abort_mask |= 32; continue;
            }
            for (int32_t i = 0; i < limit; ++i) {
                const uint64_t item = untag(items_buf[i]);
                if (item < 0x10000) continue;
                ++diag_.items_seen;
                uint64_t klass = 0;
                if (!weapon_tuning::pointer(io_, item, klass)) {
                    ++diag_.klass_read_fails; continue;
                }
                // SCALED-набор полей — у DelayPromise, голый — у
                // DelayIgnoreTimeScalePromise (см. game_offsets.h).
                if (klass == klass_scaled_ && klass_scaled_) {
                    ++diag_.klass_hits;
                    zeroed += zero_if_cadence(item, go_active::DELAYPROMISE_SCALED_TIMESPAN);
                } else if (klass == klass_unscaled_ && klass_unscaled_) {
                    ++diag_.klass_hits;
                    zeroed += zero_if_cadence(item, go_active::DELAYPROMISE_TIMESPAN);
                } else
                    note_other_klass(klass);
            }
        }
        return zeroed;
    }

    IO& io_;
    uint64_t helper_static_ = 0;   // static fields PlayerLoopHelper
    uint64_t klass_scaled_ = 0;    // Il2CppClass* UniTask.DelayPromise
    uint64_t klass_unscaled_ = 0;  // Il2CppClass* UniTask.DelayIgnoreTimeScalePromise
    State state_ = State::Waiting;
    Clock::time_point last_tick_{}, next_resolve_{};
    int zeroed_total_ = 0, last_batch_ = 0, stage_ = 0;
    Diag diag_{};
};

} // namespace fast_loot

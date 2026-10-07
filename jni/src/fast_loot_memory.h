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
// Что делает модуль: раз в 50 мс обходит
// PlayerLoopHelper.static_fields.runners (массив PlayerLoopRunner) →
// loopItems[0..tail) и у каждого промиса с точным размером 0.35f обнуляет
// delayTimeSpan. UniTask.DelayPromise.MoveNext завершает промис, как только
// elapsed >= delayTimeSpan, поэтому обнуление дотягивает его в следующем же
// кадре — весь «Забрать все» схлопывается до пары кадров. Код игры не
// трогается, пишется только одно поле в живых объектах кучи.
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
inline constexpr auto kPeriod = std::chrono::milliseconds(50);
inline constexpr auto kResolveRetry = std::chrono::seconds(2);

enum class State { Off, Waiting, Active, Failed };

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
    }

    State state() const { return state_; }
    int stage() const { return stage_; }           // где не сложился резолв (см. resolve)
    int zeroed_total() const { return zeroed_total_; }
    int last_batch() const { return last_batch_; } // обнулено за последний тик

    // Возвращает число обнулённых задержек (0 — ничего не попалось).
    int tick(uint64_t il2cpp_base, bool enabled) {
        if (!enabled) { state_ = State::Off; return 0; }
        const auto now = Clock::now();
        if (now - last_tick_ < kPeriod) return 0;
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

    // Обнуляет одну задержку, если поле всё ещё держит точную каденцию.
    int zero_if_cadence(uint64_t item, uint64_t timespan_off) {
        float timespan = 0.f, elapsed = 0.f;
        if (!io_.read(item + timespan_off, timespan) || timespan != kCadence) return 0;
        // elapsed живёт следом за полем размера в обеих раскладках: защита от
        // мусорного «попадания» в переработанный объект кучи.
        if (!io_.read(item + timespan_off + 4, elapsed)) return 0;
        if (!viewmodel::finite(elapsed) || elapsed < 0.f || elapsed > 4.f * kCadence) return 0;
        const float zero = 0.f;
        return io_.write_heap(item + timespan_off, zero) ? 1 : 0;
    }

    int sweep() {
        uint64_t runners = 0;
        if (!weapon_tuning::pointer(io_, helper_static_ + go_active::PLAYERLOOPHELPER_RUNNERS, runners) ||
            runners < 0x10000) {
            helper_static_ = 0; // статика переехала (перезапуск сцены) — резолв заново
            return 0;
        }
        int32_t runner_count = 0;
        if (!io_.read(runners + go_active::IL2CPP_ARRAY_LENGTH, runner_count) ||
            runner_count <= 0 || runner_count > kMaxRunners)
            return 0;

        int zeroed = 0;
        for (int32_t r = 0; r < runner_count; ++r) {
            uint64_t runner = 0;
            if (!weapon_tuning::pointer(
                    io_, runners + go_active::IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)r * 8, runner) ||
                runner < 0x10000)
                continue;
            int32_t tail = 0;
            if (!io_.read(runner + go_active::LOOPRUNNER_TAIL, tail) || tail <= 0) continue;
            uint64_t items = 0;
            if (!weapon_tuning::pointer(io_, runner + go_active::LOOPRUNNER_ITEMS, items) ||
                items < 0x10000)
                continue;
            int32_t capacity = 0;
            if (!io_.read(items + go_active::IL2CPP_ARRAY_LENGTH, capacity) || capacity <= 0) continue;
            const int32_t count = tail < capacity ? tail : capacity;
            const int32_t limit = count < kMaxItemsPerRunner ? count : kMaxItemsPerRunner;
            for (int32_t i = 0; i < limit; ++i) {
                uint64_t item = 0;
                if (!weapon_tuning::pointer(
                        io_, items + go_active::IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)i * 8, item) ||
                    item < 0x10000)
                    continue;
                uint64_t klass = 0;
                if (!weapon_tuning::pointer(io_, item, klass)) continue;
                // SCALED-набор полей — у DelayPromise, голый — у
                // DelayIgnoreTimeScalePromise (см. game_offsets.h).
                if (klass == klass_scaled_ && klass_scaled_)
                    zeroed += zero_if_cadence(item, go_active::DELAYPROMISE_SCALED_TIMESPAN);
                else if (klass == klass_unscaled_ && klass_unscaled_)
                    zeroed += zero_if_cadence(item, go_active::DELAYPROMISE_TIMESPAN);
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
};

} // namespace fast_loot

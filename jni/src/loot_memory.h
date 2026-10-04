#pragma once
// Быстрый сбор лута — только данные в управляемой куче.
//
// БЕЗ патчей библиотек (ни инструкций, ни констант в образе .so), БЕЗ вызовов
// кода игры и БЕЗ инжекции: единственная запись во всю игру — float в куче,
// через тот же гейт write_heap, что и у weapon_tuning.
//
// Откуда берётся задержка. Игра переносит предметы из ящика по ОДНОМУ за
// ожидание UniTask.WaitForSeconds(0.35f, ignoreTimeScale:true). Так устроены
// все три потока подбора: кнопка «Собрать всё» в окне лута
// (xw.<CollectAllItems>d__6), «быстрая загрузка» (xv.<FastLoadAll>d__5) и
// автоподбор предметов с земли (eDE.<CollectAllItems>d__6). Константа темпа —
// 0.35f из .rodata (релиз 0x213A950), у всех трёх одна и та же; задержка
// «перед подбором», которую видит игрок, — это она.
//
// Что мы пишем. Ожидание — это объект в GC-куче: UniTask.
// DelayIgnoreTimeScalePromise (темп не зависит от TimeScale; все три потока
// подбора ждут именно так) либо DelayPromise (обычный, time-scaled):
//
//   DelayIgnoreTimeScalePromise      DelayPromise (time-scaled)
//   +0x18  float delayFrameTimeSpan  +0x1C  float delayTimeSpan   // темп
//   +0x1C  float elapsed             +0x20  float elapsed
//   +0x20  int   initialFrame        +0x18  int   initialFrame
//
// Тройка полей у классов сдвинута на 4 байта — контроллер различает классы
// по имени и пользуется своей раскладкой для каждого. Тик в PlayerLoop
// завершает ожидание при elapsed >= delayTimeSpan. Дописав 0.0f в поле
// темпа активного обещания, получаем завершение ближайшим тиком: один
// предмет за игровой кадр, всё окно — за несколько кадров. Пишем ТОЛЬКО
// когда поле побитово равно 0x3EB33333 (0.35f) — любые другие ожидания
// UniTask не трогаем. Игра при каждом новом ожидании сама переписывает
// поле из скомпилированной константы, поэтому после выключения тумблера
// темп возвращается сам — восстанавливать нечего (в отличие от
// weapon_tuning здесь нет «оригиналов», которые нужно вернуть).
//
// Как находим активные обещания (всё остальное — только чтение):
//   [base+PLAYERLOOPHELPER_TYPEINFO_RVA] → Il2CppClass* PlayerLoopHelper
//     (имя «PlayerLoopHelper» / ns «Cysharp.Threading.Tasks» — сверка и кэш)
//   klass->static_fields (+IL2CPP_CLASS_STATIC_FIELDS_OFFSET)
//     + PLAYERLOOPHELPER_RUNNERS → PlayerLoopRunner[] (слот на каждый
//     PlayerLoopTiming; AddAction кладёт ожидания именно туда)
//   runners[t] (класс «PlayerLoopRunner», ns «Cysharp.Threading.Tasks.Internal»)
//     + LOOPRUNNER_ITEMS → IPlayerLoopItem[] — активные ожидания
//   элемент: класс «DelayIgnoreTimeScalePromise» или «DelayPromise» (ns
//     пустой — вложенные в UniTask; сверка и кэш по указателю класса),
//     поле темпа == 0.35f побитово, elapsed/initialFrame вразумительны.
//
// Почему это безопасно. Обещание держится массивом loopItems, пока ждёт;
// классы il2cpp не переезжают; GC на Android не уплотняет объекты. Снимок
// массива может быть устаревшим, если игра заменила его посреди обхода —
// тогда читается мусорный указатель, проверки имени/значения его отбрасывают,
// и следующий тик обходит уже новый массив. Запись мимо исключена гейтом
// write_heap (только анонимная куча /proc/pid/maps) и перечитыванием.
#include "viewmodel.h"
#include "game_offsets_active.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace fast_loot {

// Биты 0.3499999940395355f — константа темпа collect-all. Сравнение побитовое:
// у чужих ожиданий (другая длительность, NaN и т.п.) значение не совпадёт, и
// записи не будет.
inline constexpr std::uint32_t kCadenceBits = 0x3EB33333u;

// Границы одного тика: активных ожиданий обычно единицы, но повреждённый или
// замещённый посреди обхода массив не должен зациклить проход.
inline constexpr int32_t kMaxRunners      = 64;
inline constexpr std::size_t kMaxItemsPerTick = 512;
inline constexpr int32_t kMaxItemsPerArray   = 4096;

template <class IO> class Controller {
    IO& io_;
    std::uint64_t session_ = 0;
    // Кэши на сессию: классы il2cpp живут до конца процесса, поэтому вердикт
    // по указателю класса достаточно получить один раз.
    std::uint64_t helper_class_ = 0;   // PlayerLoopHelper
    std::uint64_t promise_class_ = 0;  // вложенный DelayIgnoreTimeScalePromise
    std::uint64_t scaled_class_ = 0;   // вложенный DelayPromise (time-scaled)
    std::vector<std::pair<std::uint64_t, bool>> runner_classes_;
    std::vector<std::pair<std::uint64_t, bool>> item_classes_;
    // Вращение обхода: если массив раннера больше бюджета тика, следующие
    // проходы начинаются со следующего раннера и со сдвигом по элементам,
    // чтобы один огромный массив не голодал соседей и свой хвост.
    std::uint64_t begin_runner_ = 0, item_phase_ = 0;
    std::uint32_t neutralized_ = 0;     // сколько ожиданий ускорено всего
    std::uint32_t anchor_misses_ = 0;   // self-heal after a world/PlayerLoop rebuild
    // Watchdog (заказ 04.10: «бывает перестаёт работать в один момент»).
    // Тишина по ускорениям дольше ~15 с при включённом тумблере — почти наверняка
    // что-то протухло (якорь, класс, негативный кэш, окно вращения): полный
    // сброс собирает цепочку заново, причина при этом даже не важна.
    std::uint32_t ticks_since_write_ = 0;
    std::uint32_t write_fail_streak_ = 0;
    void forget_addresses() {
        // Сессия сменилась: ни один старый адрес не обязан быть валиден.
        helper_class_ = 0; promise_class_ = 0; scaled_class_ = 0;
        begin_runner_ = item_phase_ = 0; anchor_misses_ = 0; write_fail_streak_ = 0;
        runner_classes_.clear(); item_classes_.clear();
    }
    bool pointer(std::uint64_t address, std::uint64_t& value) {
        if (!io_.read(address, value)) return false;
        value &= 0x00ffffffffffffffULL;   // pointer tags (TBI/MTE)
        return true;
    }
    // Сравнение C-строки в памяти игры: имена классов il2cpp — const char*
    // в .rodata, читаем ровно len+1 байт посимвольно (длинный блок мог бы
    // выйти за границу отображения, а кэш 128-байтных блоков гасит цену).
    // Возврат: 1 совпало, 0 читалось и чужое, -1 НЕ ЧИТАЛОСЬ (память
    // метаданных закрыта для транспорта — EIO; на устройстве пользователя
    // такие сбои идут пачками под нагрузкой, см. class_identity в game.cpp).
    int text_state(std::uint64_t address, const char* text) {
        char buf[48];
        const std::size_t len = std::strlen(text);
        if (len + 1 > sizeof(buf)) return 0;
        for (std::size_t i = 0; i <= len; ++i) {
            char c = 0;
            if (!io_.read(address + i, c)) return -1;
            if (c != text[i]) return 0;   // включая завершающий '\0'
        }
        return 1;
    }
    // Il2CppClass* по значению (в отличие от io.named, который ждёт объект):
    // имя и неймспейс лежат по указателям +0x10/+0x18 от самого класса.
    // Возврат: 1 совпало, 0 читалось и чужое, -1 имена недоступны.
    int class_state(std::uint64_t klass, const char* name, const char* ns) {
        if (klass < 0x10000) return 0;
        std::uint64_t np = 0, sp = 0;
        if (!pointer(klass + go_active::IL2CPP_CLASS_NAME, np) || np < 0x10000)
            return 0;
        if (!pointer(klass + go_active::IL2CPP_CLASS_NAMESPACE, sp) || sp < 0x10000)
            return 0;
        const int a = text_state(np, name);
        if (a <= 0) return a;
        return text_state(sp, ns);
    }
    // Кэш вердиктов «этот класс — тот самый»: имена читаются один раз на класс.
    static constexpr std::size_t kMaxClassCache = 96;
    static bool cached(const std::vector<std::pair<std::uint64_t, bool>>& cache,
                       std::uint64_t klass, bool& verdict) {
        for (const auto& e : cache)
            if (e.first == klass) { verdict = e.second; return true; }
        return false;
    }
    static void remember(std::vector<std::pair<std::uint64_t, bool>>& cache,
                         std::uint64_t klass, bool verdict) {
        if (cache.size() < kMaxClassCache) cache.emplace_back(klass, verdict);
    }
    // 0 — готово, 1 — «ещё нет» (слот пуст/не читается — игра стартует),
    // 2 — «не тот билд» (класс читается, но имя чужое).
    enum class Anchor { Ready, NotYet, Mismatch };
    Anchor resolve_helper(std::uint64_t base) {
        std::uint64_t statics = 0;
        if (helper_class_ && pointer(helper_class_ + go_active::IL2CPP_CLASS_STATIC_FIELDS_OFFSET, statics) && statics)
            return Anchor::Ready;
        helper_class_ = 0;
        // RVA ниже пересчитан по release libil2cpp.so; имя и namespace всё
        // равно проверяются до чтения statics.
        std::uint64_t klass = 0;
        if (!pointer(base + go_active::PLAYER_LOOP_HELPER_TYPEINFO_RVA, klass) || klass < 0x10000)
            return Anchor::NotYet;
        const int helper_name = class_state(klass, "PlayerLoopHelper", "Cysharp.Threading.Tasks");
        if (helper_name == 0) return Anchor::Mismatch;
        // helper_name < 0 (имена не читаются): доверяем RVA-якорю этого
        // релиза — слот TypeInfo выведен по libil2cpp, как у всех *_TYPEINFO_RVA
        // (прецедент структурного допуска — class_identity в game.cpp).
        if (!pointer(klass + go_active::IL2CPP_CLASS_STATIC_FIELDS_OFFSET, statics) || !statics)
            return Anchor::NotYet;
        helper_class_ = klass;
        return Anchor::Ready;
    }
    // Класс элемента loopItems: 0 — не обещание, 1 — DelayIgnoreTimeScalePromise
    // (темп не зависит от TimeScale; раскладка span 0x18/elapsed 0x1C/frame 0x20),
    // 2 — DelayPromise (обычный, time-scaled; span 0x1C/elapsed 0x20/frame 0x18).
    int promise_kind(std::uint64_t object) {
        std::uint64_t klass = 0;
        if (!pointer(object, klass) || klass < 0x10000) return 0;
        if (klass == promise_class_) return 1;
        if (klass == scaled_class_) return 2;
        // Окончательный отказ («читалось и чужое») кэшируется: массив раннера
        // полон не-обещаний, и без кэша имена перечитывались бы каждый тик.
        bool known_not_promise = false;
        if (cached(item_classes_, klass, known_not_promise) && known_not_promise)
            return 0;
        const int a = class_state(klass, "DelayIgnoreTimeScalePromise", "");
        if (a > 0) { promise_class_ = klass; return 1; }
        const int b = class_state(klass, "DelayPromise", "");
        if (b > 0) { scaled_class_ = klass; return 2; }
        if (a < 0 || b < 0) {
            // Имена недоступны (EIO по метаданным). ГЛАВНЫЙ ФИКС: раньше
            // нечитанное имя кэшировалось как «не обещание» НАВСЕГДА — один
            // сбой чтения убивал быстрый подбор до перетыка тумблера. Теперь
            // ничего не кэшируем и опознаём обещание ПО СТРУКТУРЕ: поле темпа
            // побитово равно 0.35f в одной из двух раскладок (это же —
            // финальная проверка именного пути, семантику она подтверждает
            // сама по себе).
            for (int kind = 1; kind <= 2; ++kind) {
                const std::uint64_t off =
                    kind == 1 ? go_active::DELAYPROMISE_TIMESPAN
                              : go_active::DELAYPROMISE_SCALED_TIMESPAN;
                std::uint32_t bits = 0;
                if (io_.read(object + off, bits) && bits == kCadenceBits)
                    return kind;
            }
            return 0;
        }
        // Читалось и чужое — окончательный отказ, кэшируем смело.
        remember(item_classes_, klass, false);
        return 0;
    }
public:
    viewmodel::Status status = viewmodel::Status::Off;
    explicit Controller(IO& io) : io_(io) {}
    std::uint32_t neutralized() const { return neutralized_; }
    bool has_state() const { return session_ != 0; }
    void reset() {
        // Выключение тумблера/отвязка: поле темпа переписывает сама игра при
        // каждом новом ожидании, так что возвращать «оригиналы» не нужно —
        // просто всё забываем.
        forget_addresses();
        session_ = 0; neutralized_ = 0; ticks_since_write_ = 0;
        status = viewmodel::Status::Off;
    }
    void tick(std::uint64_t session, std::uint64_t base, bool enabled) {
        using viewmodel::Status;
        if (!enabled) { reset(); return; }
        if (session != session_) { forget_addresses(); session_ = session; }
        if (!base) { status = Status::Waiting; return; }
        const Anchor anchor = resolve_helper(base);
        if (anchor == Anchor::NotYet)   { status = Status::Waiting; return; }
        if (anchor == Anchor::Mismatch) { status = Status::Failed; return; }

        std::uint64_t statics = 0, runners = 0;
        if (!pointer(helper_class_ + go_active::IL2CPP_CLASS_STATIC_FIELDS_OFFSET, statics) ||
            !statics ||
            !pointer(statics + go_active::PLAYERLOOPHELPER_RUNNERS, runners) ||
            !runners) {
            // PlayerLoop/UniTask statics may be replaced during respawn.  Do
            // not retain a dead helper and negative class cache indefinitely
            // (switching the toggle used to be the only way to clear them).
            if (++anchor_misses_ >= 30) forget_addresses();
            status = Status::Waiting;
            return;
        }
        int32_t runners_len = 0;
        if (!io_.read(runners + go_active::IL2CPP_ARRAY_LENGTH, runners_len) ||
            runners_len <= 0 || runners_len > kMaxRunners) {
            if (++anchor_misses_ >= 30) forget_addresses();
            status = Status::Waiting;   // снимок заменился — попробуем в следующий тик
            return;
        }
        anchor_misses_ = 0;

        std::uint32_t writes = 0;
        bool write_failed = false;
        std::size_t budget = kMaxItemsPerTick;
        const std::uint64_t begin = begin_runner_ % (std::uint64_t)runners_len;
        begin_runner_ = begin + 1;
        item_phase_ += kMaxItemsPerTick;   // окно элементов едет вперёд каждый тик
        for (std::uint64_t step = 0; step < (std::uint64_t)runners_len && budget > 0; ++step) {
            const std::uint64_t t = (begin + step) % (std::uint64_t)runners_len;
            std::uint64_t runner = 0;
            if (!pointer(runners + go_active::IL2CPP_ARRAY_FIRST_ELEMENT +
                             (std::uint64_t)t * 8, runner) ||
                !runner)
                continue;
            // Класс раннера: сверяется один раз на класс. Не опознан — это
            // не раннер (снимок устарел или поле в этом билде другое): молча
            // пропускаем, статистики «не применено» здесь не делаем — семантику
            // подтверждает побитовая сверка 0.35f в самом обещании.
            std::uint64_t rk = 0;
            if (!pointer(runner, rk) || rk < 0x10000) continue;
            bool runner_ok = false;
            bool runner_cached = cached(runner_classes_, rk, runner_ok);
            if (!runner_cached) {
                const int st = class_state(rk, "PlayerLoopRunner",
                                           "Cysharp.Threading.Tasks.Internal");
                if (st < 0) {
                    // Имена не читаются: не кэшируем отказ (EIO преходящ) и
                    // проверяем раннера по структуре — валидный массив
                    // элементов с вразумительной длиной. Запись всё равно
                    // защищена побитовой сверкой 0.35f в самом обещании.
                    std::uint64_t probe_items = 0;
                    int32_t probe_len = -1;
                    if (pointer(runner + go_active::LOOPRUNNER_ITEMS, probe_items) &&
                        probe_items &&
                        io_.read(probe_items + go_active::IL2CPP_ARRAY_LENGTH, probe_len) &&
                        probe_len >= 0 && probe_len <= kMaxItemsPerArray)
                        runner_ok = true;
                } else {
                    runner_ok = (st > 0);
                    remember(runner_classes_, rk, runner_ok);
                }
            }
            if (!runner_ok) continue;
            std::uint64_t items = 0;
            if (!pointer(runner + go_active::LOOPRUNNER_ITEMS, items) || !items) continue;
            int32_t count = 0;
            if (!io_.read(items + go_active::IL2CPP_ARRAY_LENGTH, count) ||
                count < 0 || count > kMaxItemsPerArray)
                continue;
            // Живой счётчик раннера — tail (+0x30), длина массива — только
            // ёмкость: после компакции RunCore в хвосте capacity лежат
            // устаревшие указатели (в т.ч. на промисы, вернувшиеся в пул и
            // переиспользованные чужими ожиданиями) — их не трогаем. Ошибка
            // чтения tail (старая раскладка) — откат к длине массива.
            int32_t tail = -1;
            if (io_.read(runner + go_active::LOOPRUNNER_TAIL, tail) &&
                tail >= 0 && (std::size_t)tail <= (std::size_t)count)
                count = tail;
            const std::size_t n =
                (std::size_t)count < budget ? (std::size_t)count : budget;
            // Обычный массив (count <= бюджета) проходится целиком с любого
            // сдвига; огромный — окном, которое каждый тик едет вперёд.
            const std::uint64_t phase = count > 0 ? item_phase_ % (std::uint64_t)count : 0;
            for (std::size_t k = 0; k < n; ++k) {
                const std::uint64_t i = (phase + k) % (std::uint64_t)count;
                std::uint64_t item = 0;
                if (!pointer(items + go_active::IL2CPP_ARRAY_FIRST_ELEMENT + i * 8,
                             item) ||
                    !item)
                    continue;
                const int kind = promise_kind(item);
                if (kind == 0) continue;
                // Раскладка полей зависит от класса: у time-scaled варианта
                // (DelayPromise) тройка сдвинута на 4 байта.
                const std::uint64_t span_off =
                    kind == 1 ? go_active::DELAYPROMISE_TIMESPAN
                              : go_active::DELAYPROMISE_SCALED_TIMESPAN;
                const std::uint64_t elapsed_off =
                    kind == 1 ? go_active::DELAYPROMISE_ELAPSED
                              : go_active::DELAYPROMISE_SCALED_ELAPSED;
                const std::uint64_t frame_off =
                    kind == 1 ? go_active::DELAYPROMISE_INITIAL_FRAME
                              : go_active::DELAYPROMISE_SCALED_INITIAL_FRAME;
                // Поле темпа: побитово 0.35f — иначе это чужое ожидание.
                std::uint32_t span_bits = 0;
                if (!io_.read(item + span_off, span_bits) ||
                    span_bits != kCadenceBits)
                    continue;
                float elapsed = 0.f;
                std::int32_t frame = 0;
                if (!io_.read(item + elapsed_off, elapsed) ||
                    !viewmodel::finite(elapsed) || elapsed < 0.f || elapsed > 0.36f)
                    continue;
                if (!io_.read(item + frame_off, frame) ||
                    frame < 0 || frame > 2000000000)
                    continue;
                if (!io_.write_heap(item + span_off, 0.f) ||
                    !io_.read(item + span_off, span_bits) ||
                    span_bits != 0u) {
                    write_failed = true;
                    continue;
                }
                ++writes;
            }
            budget -= n;
        }
        neutralized_ += writes;
        if (writes) {
            ticks_since_write_ = 0;
            write_fail_streak_ = 0;
        } else if (++ticks_since_write_ >= 900) {
            // ~15 с без единого ускоренного ожидания при включённом тумблере:
            // сбрасываем всё (окно вращения в т.ч.) — пересборка стоит копейки,
            // а лечит любую протухшую опору, включая неугаданную.
            forget_addresses();
            ticks_since_write_ = 0;
        }
        if (write_failed && ++write_fail_streak_ >= 4) {
            // Записи отбиваются подряд — страница/карта кучи могла смениться;
            // полный сброс, чтобы следующий тик пошёл по свежей цепочке.
            forget_addresses();
            write_fail_streak_ = 0;
        }
        status = write_failed ? Status::Failed : Status::Active;
    }
};
}  // namespace fast_loot

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
// Почему подбор работал «через раз» (разбор 27.09):
//  1. elapsed сверялся с потолком 0.36 с. При длинном игровом кадре (на
//     этом устройстве встречаются пики 0.5-0.9 с) elapsed успевает
//     перескочить потолок ДО нашего первого тика после создания ожидания —
//     и валидное ожидание 0.35 отбрасывалось вовсе. Потолок поднят до
//     0.60 с: роль проверки — отсечь мусор разорванного снимка, а не
//     измерять кадры игры.
//  2. Бюджет элементов был ОБЩИМ на тик: один гигантский loopItems в
//     соседнем раннере съедал все 512 слотов, и ожидания сбора в своём
//     раннере не попадали на запись несколько циклов. Теперь бюджет — на
//     каждый раннер свой.
//
// Как находим активные обещания (всё остальное — только чтение):
//   [base+PLAYERLOOPHELPER_TYPEINFO_RVA] → Il2CppClass* PlayerLoopHelper
//     (имя «PlayerLoopHelper» / ns «Cysharp.Threading.Tasks» — сверка и кэш)
//   klass->static_fields (+IL2CPP_CLASS_STATIC_FIELDS)
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

// Статический поиск классов по TYPEINFO_RVA.
std::uint64_t game_static_class(const char* name, const char* ns);
std::uint64_t game_static_class_nth(const char* name, const char* ns, int nth);

namespace fast_loot {

// Биты 0.3499999940395355f — константа темпа collect-all. Сравнение побитовое:
// у чужих ожиданий (другая длительность, NaN и т.п.) значение не совпадёт, и
// записи не будет.
inline constexpr std::uint32_t kCadenceBits = 0x3EB33333u;

// Границы одного тика: активных ожиданий обычно единицы, но повреждённый или
// замещённый посреди обхода массив не должен зациклить проход. Бюджет
// элементов — на РАННЕР (а не на тик): один большой массив не должен
// голодать ожидания в соседних раннерах.
inline constexpr int32_t kMaxRunners      = 64;
inline constexpr std::size_t kMaxItemsPerTick = 512;
inline constexpr int32_t kMaxItemsPerArray   = 4096;

// Потолок elapsed живого ожидания 0.35 с: запас = длинный игровой кадр
// (на устройстве встречались пики полсекунды и больше). Мусор разорванного
// снимка отсекается лежащим выше потолком (см. обход).
inline constexpr float kElapsedMax = 0.60f;

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
    void forget_addresses() {
        // Сессия сменилась: ни один старый адрес не обязан быть валиден.
        helper_class_ = 0; promise_class_ = 0; scaled_class_ = 0;
        begin_runner_ = item_phase_ = 0;
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
    bool text_is(std::uint64_t address, const char* text) {
        char buf[48];
        const std::size_t len = std::strlen(text);
        if (len + 1 > sizeof(buf)) return false;
        for (std::size_t i = 0; i <= len; ++i) {
            char c = 0;
            if (!io_.read(address + i, c)) return false;
            if (c != text[i]) return false;   // включая завершающий '\0'
        }
        return true;
    }
    // Il2CppClass* по значению (в отличие от io.named, который ждёт объект):
    // имя и неймспейс лежат по указателям +0x10/+0x18 от самого класса.
    bool class_is(std::uint64_t klass, const char* name, const char* ns) {
        if (klass < 0x10000) return false;
        std::uint64_t np = 0, sp = 0;
        if (!pointer(klass + go_active::IL2CPP_CLASS_NAME, np) || np < 0x10000)
            return false;
        if (!pointer(klass + go_active::IL2CPP_CLASS_NAMESPACE, sp) || sp < 0x10000)
            return false;
        return text_is(np, name) && text_is(sp, ns);
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
        if (helper_class_ && pointer(helper_class_ + go_active::IL2CPP_CLASS_STATIC_FIELDS, statics) && statics)
            return Anchor::Ready;
        helper_class_ = 0; // named class with empty statics may be an uninitialized copy
        bool saw_named = false;
        for (int n = 0; n < 8; ++n) {
            const std::uint64_t found = game_static_class_nth(
                "PlayerLoopHelper", "Cysharp.Threading.Tasks", n);
            if (!found) break;
            if (!class_is(found, "PlayerLoopHelper", "Cysharp.Threading.Tasks")) continue;
            saw_named = true;
            if (!pointer(found + go_active::IL2CPP_CLASS_STATIC_FIELDS, statics) || !statics) continue;
            helper_class_ = found;
            return Anchor::Ready;
        }
        std::uint64_t klass = 0;
        if (!pointer(base + go_active::PLAYERLOOPHELPER_TYPEINFO_RVA, klass) || klass < 0x10000)
            return Anchor::NotYet;
        if (!class_is(klass, "PlayerLoopHelper", "Cysharp.Threading.Tasks"))
            return saw_named ? Anchor::NotYet : Anchor::Mismatch;
        if (!pointer(klass + go_active::IL2CPP_CLASS_STATIC_FIELDS, statics) || !statics)
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
        bool verdict = false;
        if (!cached(item_classes_, klass, verdict)) {
            verdict = class_is(klass, "DelayIgnoreTimeScalePromise", "") ||
                      class_is(klass, "DelayPromise", "");
            remember(item_classes_, klass, verdict);
        }
        if (!verdict) return 0;
        if (class_is(klass, "DelayIgnoreTimeScalePromise", "")) {
            promise_class_ = klass; return 1;
        }
        scaled_class_ = klass; return 2;
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
        session_ = 0; neutralized_ = 0;
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
        if (!pointer(helper_class_ + go_active::IL2CPP_CLASS_STATIC_FIELDS, statics) ||
            !statics ||
            !pointer(statics + go_active::PLAYERLOOPHELPER_RUNNERS, runners) ||
            !runners) {
            status = Status::Waiting;   // UniTask ещё не инициализировался
            return;
        }
        int32_t runners_len = 0;
        if (!io_.read(runners + go_active::IL2CPP_ARRAY_LENGTH, runners_len) ||
            runners_len <= 0 || runners_len > kMaxRunners) {
            status = Status::Waiting;   // снимок заменился — попробуем в следующий тик
            return;
        }

        std::uint32_t writes = 0;
        bool write_failed = false;
        const std::uint64_t begin = begin_runner_ % (std::uint64_t)runners_len;
        begin_runner_ = begin + 1;
        item_phase_ += kMaxItemsPerTick;   // окно элементов едет вперёд каждый тик
        for (std::uint64_t step = 0; step < (std::uint64_t)runners_len; ++step) {
            // Бюджет — на РАННЕР, а не на тик: огромный loopItems в одном
            // раннере не должен неделями голодать ожидания сбора в соседнем
            // (это и был один из сценариев «подбор через раз»).
            std::size_t budget = kMaxItemsPerTick;
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
            if (!cached(runner_classes_, rk, runner_ok)) {
                runner_ok = class_is(rk, "PlayerLoopRunner",
                                     "Cysharp.Threading.Tasks.Internal");
                remember(runner_classes_, rk, runner_ok);
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
                // Потолок kElapsedMax (0.60 с), а не 0.35+кадр: при длинном
                // игровом кадре elapsed перескакивает 0.35 ДО нашего первого
                // тика, и прежний порог 0.36 навсегда отбрасывал живое
                // ожидание — один из сценариев «подбор через раз».
                if (!io_.read(item + elapsed_off, elapsed) ||
                    !viewmodel::finite(elapsed) || elapsed < 0.f ||
                    elapsed > kElapsedMax)
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
        status = write_failed ? Status::Failed : Status::Active;
    }
};
}  // namespace fast_loot

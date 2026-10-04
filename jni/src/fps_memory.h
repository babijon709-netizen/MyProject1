#pragma once
#include "viewmodel_memory.h"
#include "fps_unity_scan.h"

namespace viewmodel {
struct FrameRate { int32_t value; };
inline bool valid(FrameRate v) { return v.value >= -1 && v.value <= 1000; }
// Вид подключения движка к Android frame pacing: 0 — выключен, 1 — нативный
// драйвер (Swappy), 2 — через JNI. Реальный темп кадров задаёт pacing-драйвер,
// а поле targetFrameRate читается только расчётом без pacing. Один прямой
// int32-патч поэтому не менял темп: интервал оставался с прошлой установки
// setter'а («при 90 делает больше») или blocking path вообще не читался
// («иногда не срабатывает»). Значения вне 0..2 не перезаписываем.
struct PaceKind { int32_t value; };
inline bool valid(PaceKind v) { return v.value >= 0 && v.value <= 2; }

// libunity.so, not libil2cpp.so. Exact GNU build ID of the supported release;
// любой другой движок проходит через поиск раскладки на месте (см. ниже
// fps_unity_scan.h), а не отваливается молча. See FPS144.md.
struct FpsLayout {
    uint64_t build_id, getter, target;
    uint32_t load_instruction;
    uint64_t pacing_update;  // функция из setter'а: str x30,[sp,#-0x20]! (0xf81e0ffe)
    uint64_t pacing_type;    // int32: 0 off / 1 native / 2 jni; 0 = поле неизвестно
};
inline constexpr FpsLayout kFpsLayouts[] = {
    // Релиз: GNU build ID 7eb634edc8dede69 (u64 LE, как читается из памяти).
    {0x69dedec8ed34b67eULL, 0x7390e0, 0x17b8184, 0xb9418500, 0x8fcac8, 0x183d618},
    // Бета: GNU build ID 9417acde63f688ed — getter той же формы на той же
    // странице (adrp x8 -> 0x17b8000), но ldr w0,[x8,#0xa74] и свои RVA
    // pacing-полей (см. FPS144.md, таблица «Подтверждённые native-адреса»).
    {0xed88f663deac1794ULL, 0x7392f0, 0x17b8a74, 0xb94a7500, 0x8fccd8, 0x183df18},
};
// GNU build ID образа (u64 LE, как он лежит в .note.gnu.build-id).
template<class IO> uint64_t fps_build_id(IO& io, uint64_t base) {
    uint64_t id = 0;
    if (!base || !io.read(base + 0x318, id)) return 0;
    return id;
}

// Раскладка, найденная сканом файла libunity на этом устройстве (см.
// fps_unity_scan.h). Пустая, пока скан не дал однозначного кандидата.
inline FpsLayout& discovered_fps_layout() {
    static FpsLayout layout{};
    return layout;
}

// Живая проверка раскладки: три инструкции геттера на месте и раскодируются
// ровно в то поле, которое мы собираемся писать.
template<class IO> bool fps_layout_live(IO& io, uint64_t base, const FpsLayout& layout) {
    if (!base || !layout.getter || !layout.target) return false;
    std::array<uint32_t, 3> code{};
    if (!io.read(base + layout.getter, code)) return false;
    if (code[1] != layout.load_instruction) return false;
    uint64_t target = 0;
    if (!decode_int32_getter(layout.getter, code.data(), target)) return false;
    if (target != layout.target) return false;
    if (!layout.pacing_update) return true;   // найденная раскладка: pacing не трогаем
    uint32_t pacing_head = 0;
    return io.read(base + layout.pacing_update, pacing_head) && pacing_head == 0xf81e0ffe;
}

template<class IO> const FpsLayout* fps_layout(IO& io, uint64_t base) {
    if (!base) return nullptr;
    const uint64_t id = fps_build_id(io, base);
    if (!id) return nullptr;
    for (const auto& layout : kFpsLayouts) {
        if (id != layout.build_id) continue;
        std::array<uint32_t, 3> code{};
        uint32_t pacing_head = 0;
        if (io.read(base + layout.getter, code) && code[0] == 0xf00083e8 &&
            code[1] == layout.load_instruction && code[2] == 0xd65f03c0 &&
            io.read(base + layout.pacing_update, pacing_head) && pacing_head == 0xf81e0ffe)
            return &layout;
    }
    // Движок не из таблицы (обновился Unity): берём раскладку, найденную
    // сканом, но только если она найдена для ЭТОГО образа и всё ещё на месте.
    const FpsLayout& found = discovered_fps_layout();
    if (found.build_id && found.build_id == id && fps_layout_live(io, base, found))
        return &found;
    return nullptr;
}

template<class IO> class FpsController {
    IO& io;
    Patch<FrameRate> cap_;
    Patch<PaceKind> pace_;
    uint64_t session_ = 0, base_ = 0;
    // Последнее подтверждённое содержимое памяти — для отображения в меню.
    int32_t field_seen_ = 0, pace_seen_ = 0;
    // Подряд идущие тики, в которых игра вернула своё значение поверх нашего.
    int contested_ = 0;
public:
    Status status = Status::Off;
    explicit FpsController(IO& memory) : io(memory) {}
    bool has_state() const { return cap_.written || pace_.written; }
    int32_t field_seen() const { return field_seen_; }
    int32_t pace_seen() const { return pace_seen_; }
    // Игра переписывает targetFrameRate сама (своя настройка графики
    // применяется каждый кадр). Тогда раз в 100 мс поправлять поле мало:
    // большую часть кадров движок видит значение игры, и предел не меняется.
    bool contested() const { return contested_ >= 2; }
    void reset(bool restore) {
        if (restore && fps_layout(io, base_)) { cap_.restore(io); pace_.restore(io); }
        cap_ = {}; pace_ = {}; base_ = session_ = 0; status = Status::Off;
        field_seen_ = pace_seen_ = 0; contested_ = 0;
    }
    void tick(uint64_t session, uint64_t base, int requested_limit) {
        const int limit = sanitize_fps_limit(requested_limit);
        const bool enabled = limit != 0;
        // Session change means the old address space is no longer ours.
        if (session_ != session || base_ != base) {
            reset(false); session_ = session; base_ = base;
        }
        status = enabled ? Status::Waiting : Status::Off;
        if (!enabled && !has_state()) return;
        if (!base) return;
        const auto* layout = fps_layout(io, base);
        if (!layout) { status = Status::Failed; return; }
        if (!enabled) {
            bool ok = true;
            if (cap_.written) ok &= cap_.restore(io);
            if (pace_.written) ok &= pace_.restore(io);
            status = ok ? Status::Off : Status::Failed;
            return;
        }
        // Refresh snapshots when the game changes its graphics preset. Never
        // accumulate our own writes; never overwrite a new preset on disable.
        // Запись только при отклонении от выбранного значения: в установившемся
        // режиме контроллер не пишет в процесс вообще.
        bool ok = cap_.sample(io, base + layout->target);
        FrameRate current{};
        if (ok) ok = io.read(base + layout->target, current);
        if (ok && current.value != limit) {
            // Наше значение сбили: это игра, а не мы (свои записи контроллер
            // не теряет). Считаем такие тики — по ним ускоряется повтор.
            if (cap_.written && contested_ < 1000) ++contested_;
            ok = cap_.put(io, FrameRate{limit});
        } else if (ok) {
            contested_ = 0;
        }
        if (ok) { ok = io.read(base + layout->target, current); if (ok) field_seen_ = current.value; }
        // Pacing-драйвер глушим нулём: иначе его интервал из прошлой установки
        // сеттера задаёт темп поверх нашего targetFrameRate. Для раскладки,
        // найденной сканом, поле pacing неизвестно (layout->pacing_type == 0):
        // тогда пишем только предел кадров — лучше, чем ничего, и уж точно
        // лучше, чем обнулять случайную глобальную переменную движка.
        const bool have_pacing = layout->pacing_type != 0;
        if (ok && have_pacing) {
            ok = pace_.sample(io, base + layout->pacing_type);
            PaceKind pace{};
            if (ok) ok = io.read(base + layout->pacing_type, pace);
            if (ok && pace.value != 0) ok = pace_.put(io, PaceKind{0});
            if (ok) { ok = io.read(base + layout->pacing_type, pace); if (ok) pace_seen_ = pace.value; }
        }
        if (!ok) { status = Status::Failed; return; }
        status = (field_seen_ == limit && (!have_pacing || pace_seen_ == 0))
                     ? Status::Active : Status::Failed;
    }
};
} // namespace viewmodel

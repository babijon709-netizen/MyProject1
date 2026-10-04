#pragma once
#include "viewmodel_memory.h"
#include <cstdint>

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
// fail closed on any other engine. See FPS144.md.
struct FpsLayout {
    uint64_t build_id, getter, target;
    uint32_t load_instruction;
    uint64_t pacing_update;  // функция из setter'а: str x30,[sp,#-0x20]! (0xf81e0ffe)
    uint64_t pacing_type;    // int32: 0 off / 1 native / 2 jni
};
inline constexpr FpsLayout kFpsLayouts[] = {
    // Релиз: GNU build ID 7eb634edc8dede69 (u64 LE, как читается из памяти).
    {0x69dedec8ed34b67eULL, 0x7390e0, 0x17b8184, 0xb9418500, 0x8fcac8, 0x183d618},
    // Бета: GNU build ID 9417acde63f688ed — getter той же формы на той же
    // странице (adrp x8 -> 0x17b8000), но ldr w0,[x8,#0xa74] и свои RVA
    // pacing-полей (см. FPS144.md, таблица «Подтверждённые native-адреса»).
    {0xed88f663deac1794ULL, 0x7392f0, 0x17b8a74, 0xb94a7500, 0x8fccd8, 0x183df18},
    // Текущая сборка (обновление игры): GNU build ID e84c8714aff1d6e1.
    // getter/setter Application.targetFrameRate съехали: getter 0x73cbe0
    // (adrp x8 -> 0x17d3000; ldr w0,[x8,#0x54]; ret), setter 0x73cbec отдаёт
    // хвостом в UpdateFramePacing 0x9005d4 (через трамплин 0x904114), которая
    // читает pacing kind ldr w8,[x21,#0x658] при x21=0x1858000. CBZ по этому
    // полю (0) гасит расчёт pacing — как и раньше зануляем его.
    {0xe84c8714aff1d6e1ULL, 0x73cbe0, 0x17d3054, 0xb9405500, 0x9005d4, 0x1858658},
};

// Жёсткой проверке точного слова adrp (0xf00083e8) обновлённая сборка не
// соответствует: тот же механизм, другая страница поля. Верифицируем форму
// семантически: adrp x8 даёт страницу поля, ldr w0,[x8,#imm] адресует само
// поле (должно совпасть с layout.target), за ними ret. Строгость опознания
// та же: всё ещё fail closed по build ID + форме кода.
inline bool fps_decode_adrp(uint32_t w, uint64_t pc, int& rd, uint64_t& page) {
    if ((w & 0x1F000000u) != 0x10000000u || !(w & 0x80000000u)) return false;
    rd = (int)(w & 31u);
    uint64_t imm = ((uint64_t)((w >> 5) & 0x7FFFFu) << 2) | ((w >> 29) & 3u);
    int64_t simm = (int64_t)(imm << 43) >> 43;   // sign-extend 21 бит
    page = (pc & ~0xFFFULL) + ((uint64_t)simm << 12);
    return true;
}
inline bool fps_decode_ldr32(uint32_t w, int& rt, int& rn, uint64_t& offset) {
    if ((w & 0xFFC00000u) != 0xB9400000u) return false;  // ldr wt, [xn, #imm]
    rt = (int)(w & 31u);
    rn = (int)((w >> 5) & 31u);
    offset = (uint64_t)((w >> 10) & 0xFFFu) << 2;
    return true;
}

template<class IO> const FpsLayout* fps_layout(IO& io, uint64_t base) {
    if (!base) return nullptr;
    uint64_t id = 0;
    if (!io.read(base + 0x318, id)) return nullptr;
    for (const auto& layout : kFpsLayouts) {
        if (id != layout.build_id) continue;
        std::array<uint32_t, 3> code{};
        uint32_t pacing_head = 0;
        if (!io.read(base + layout.getter, code) ||
            !io.read(base + layout.pacing_update, pacing_head) ||
            pacing_head != 0xf81e0ffe || code[2] != 0xd65f03c0) continue;
        int rd = -1, rt = -1, rn = -1;
        uint64_t page = 0, offset = 0;
        if (!fps_decode_adrp(code[0], base + layout.getter, rd, page)) continue;
        if (!fps_decode_ldr32(code[1], rt, rn, offset)) continue;
        // Точное слово load_instruction из таблицы И адрес из декодера обязаны
        // сговориться с полем target — любое отклонение = не опознано.
        if (code[1] != layout.load_instruction || rt != 0 || rd != rn ||
            page + offset != base + layout.target) continue;
        return &layout;
    }
    return nullptr;
}

template<class IO> class FpsController {
    IO& io;
    Patch<FrameRate> cap_;
    Patch<PaceKind> pace_;
    uint64_t session_ = 0, base_ = 0;
    // Последнее подтверждённое содержимое памяти — для отображения в меню.
    int32_t field_seen_ = 0, pace_seen_ = 0;
public:
    Status status = Status::Off;
    explicit FpsController(IO& memory) : io(memory) {}
    bool has_state() const { return cap_.written || pace_.written; }
    int32_t field_seen() const { return field_seen_; }
    int32_t pace_seen() const { return pace_seen_; }
    void reset(bool restore) {
        if (restore && fps_layout(io, base_)) { cap_.restore(io); pace_.restore(io); }
        cap_ = {}; pace_ = {}; base_ = session_ = 0; status = Status::Off;
        field_seen_ = pace_seen_ = 0;
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
        if (ok && current.value != limit) ok = cap_.put(io, FrameRate{limit});
        if (ok) { ok = io.read(base + layout->target, current); if (ok) field_seen_ = current.value; }
        // Pacing-драйвер глушим нулём: иначе его интервал из прошлой установки
        // сеттера задаёт темп поверх нашего targetFrameRate.
        if (ok) {
            ok = pace_.sample(io, base + layout->pacing_type);
            PaceKind pace{};
            if (ok) ok = io.read(base + layout->pacing_type, pace);
            if (ok && pace.value != 0) ok = pace_.put(io, PaceKind{0});
            if (ok) { ok = io.read(base + layout->pacing_type, pace); if (ok) pace_seen_ = pace.value; }
        }
        if (!ok) { status = Status::Failed; return; }
        status = (field_seen_ == limit && pace_seen_ == 0) ? Status::Active : Status::Failed;
    }
};
} // namespace viewmodel

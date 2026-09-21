#pragma once
#include "viewmodel_memory.h"

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

// libunity.so, not libil2cpp.so. Exact GNU build IDs from the supplied
// release/beta archives; fail closed on any other engine. See FPS144.md.
struct FpsLayout {
    uint64_t build_id, getter, target;
    uint32_t load_instruction;
    uint64_t pacing_update;  // функция из setter'а: str x30,[sp,#-0x20]! (0xf81e0ffe)
    uint64_t pacing_type;    // int32: 0 off / 1 native / 2 jni
};
inline constexpr FpsLayout kFpsLayouts[] = {
    {0x69dedec8ed34b67eULL, 0x7390e0, 0x17b8184, 0xb9418500, 0x8fcac8, 0x183d618},
    {0xed88f663deac1794ULL, 0x7392f0, 0x17b8a74, 0xb94a7500, 0x8fccd8, 0x183df18},
};
template<class IO> const FpsLayout* fps_layout(IO& io, uint64_t base) {
    if (!base) return nullptr;
    uint64_t id = 0;
    if (!io.read(base + 0x318, id)) return nullptr;
    for (const auto& layout : kFpsLayouts) {
        if (id != layout.build_id) continue;
        std::array<uint32_t, 3> code{};
        uint32_t pacing_head = 0;
        if (io.read(base + layout.getter, code) && code[0] == 0xf00083e8 &&
            code[1] == layout.load_instruction && code[2] == 0xd65f03c0 &&
            io.read(base + layout.pacing_update, pacing_head) && pacing_head == 0xf81e0ffe)
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

#pragma once
#include "viewmodel_memory.h"

namespace viewmodel {
struct FrameRate { int32_t value; };
inline bool valid(FrameRate v) { return v.value >= -1 && v.value <= 1000; }

// libunity.so, not libil2cpp.so. Exact GNU build IDs from the supplied
// release/beta archives; fail closed on any other engine. See FPS144.md.
struct FpsLayout {
    uint64_t build_id, getter, target;
    uint32_t load_instruction;
};
inline constexpr FpsLayout kFpsLayouts[] = {
    {0x69dedec8ed34b67eULL, 0x7390e0, 0x17b8184, 0xb9418500},
    {0xed88f663deac1794ULL, 0x7392f0, 0x17b8a74, 0xb94a7500},
};
template<class IO> const FpsLayout* fps_layout(IO& io, uint64_t base) {
    if (!base) return nullptr;
    uint64_t id = 0;
    if (!io.read(base + 0x318, id)) return nullptr;
    for (const auto& layout : kFpsLayouts) {
        if (id != layout.build_id) continue;
        std::array<uint32_t, 3> code{};
        if (io.read(base + layout.getter, code) && code[0] == 0xf00083e8 &&
            code[1] == layout.load_instruction && code[2] == 0xd65f03c0)
            return &layout;
    }
    return nullptr;
}

template<class IO> class FpsController {
    IO& io;
    Patch<FrameRate> cap_;
    uint64_t session_ = 0, base_ = 0;
public:
    Status status = Status::Off;
    explicit FpsController(IO& memory) : io(memory) {}
    bool has_state() const { return cap_.written; }
    void reset(bool restore) {
        if (restore && fps_layout(io, base_)) cap_.restore(io);
        cap_ = {}; base_ = session_ = 0; status = Status::Off;
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
            status = cap_.restore(io) ? Status::Off : Status::Failed;
            return;
        }
        // Refresh snapshots when the game changes its graphics preset. Never
        // accumulate our own writes; never overwrite a new preset on disable.
        if (!cap_.sample(io, base + layout->target) || !cap_.put(io, FrameRate{limit})) {
            status = Status::Failed; return;
        }
        FrameRate applied{};
        status = io.read(base + layout->target, applied) && applied.value == limit
            ? Status::Active : Status::Failed;
    }
};
} // namespace viewmodel

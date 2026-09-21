#pragma once
#include "viewmodel.h"
#include <array>

namespace fast_loot {
// Only the CollectAllItems inter-item wait is changed. Inventory commands,
// capacity checks, cancellation and the separate FastLoadAll (deposit) path
// are untouched. Exact images and consumer evidence: FAST_LOOT.md.
struct Layout {
    std::array<uint8_t, 20> build_id;
    uint64_t site;
    std::array<uint32_t, 8> guard; // site-12 through the WaitForSeconds call
};
inline constexpr Layout kLayouts[] = {
    {{0xd8,0x5e,0x63,0xcd,0x8d,0xcb,0xd4,0x81,0x69,0x50,0x12,0xef,0x2e,0x28,0x4f,0x0e,0xeb,0x9e,0xf3,0x07},
     0x5b5cad4, {0xd0fe2ee8,0x52800020,0x52800101,0xbd495100,0xaa1403e2,0x2a1f03e3,0xaa1f03e4,0x959f3174}},
    {{0xe1,0x07,0x95,0x34,0x9b,0xd7,0xc2,0xcd,0xc7,0xd8,0x52,0x75,0x97,0xc6,0x43,0x95,0xa4,0xef,0xed,0x79},
     0x5af4d04, {0xf0fe3208,0x52800020,0x52800101,0xbd400100,0xaa1403e2,0x2a1f03e3,0xaa1f03e4,0x95a05a57}},
};
// One baseline AArch64 instruction: FMOV S0,WZR supplies exact float +0.0.
// WaitForSeconds receives zero; its normal PlayerLoop/cancellation semantics
// remain in place. No FP16 requirement, shared-pool writes or multi-word patch.
inline constexpr uint32_t kFastInstruction = 0x1e2703e0;
inline constexpr unsigned kDelayMilliseconds = 0;
inline bool instruction_supported() {
#if defined(__aarch64__)
    return true;
#else
    return false;
#endif
}

template<class IO> class Controller {
    IO& io;
    const bool instruction_supported_;
    uint64_t session_ = 0, base_ = 0;
    bool owned_ = false;
public:
    viewmodel::Status status = viewmodel::Status::Off;
    explicit Controller(IO& memory, bool supported) : io(memory), instruction_supported_(supported) {}
    bool has_state() const { return owned_; }
    bool reset(bool restore) {
        if (restore && owned_) tick(session_, base_, false);
        const bool ok = !restore || !owned_;
        owned_ = false; session_ = base_ = 0; status = viewmodel::Status::Off;
        return ok;
    }
    void tick(uint64_t session, uint64_t base, bool enabled) {
        using viewmodel::Status;
        if (session != session_ || base != base_) {
            reset(false); session_ = session; base_ = base;
        }
        status = enabled ? Status::Waiting : Status::Off;
        if (!enabled && !owned_) return;
        if (!base) return;
        status = Status::Failed;
        if (base & 3u) return;
        if (enabled && !instruction_supported_) return; // never risk SIGILL
        std::array<uint8_t, 20> id{};
        if (!io.read(base + 0x318, id)) return;
        const Layout* layout = nullptr;
        for (const auto& candidate : kLayouts) {
            if (id == candidate.build_id) { layout = &candidate; break; }
        }
        if (!layout) return; // no scans or guessed offsets on unknown versions
        std::array<uint32_t, 8> code{};
        if (!io.read(base + layout->site - 12, code)) return;
        for (size_t i = 0; i < code.size(); ++i) {
            if (i != 3 && code[i] != layout->guard[i]) return;
        }
        const uint32_t original = layout->guard[3];
        if (code[3] != original && (code[3] != kFastInstruction || !owned_)) {
            // Another writer owns this instruction. Never overwrite its code.
            owned_ = false;
            return;
        }
        const uint32_t desired = enabled ? kFastInstruction : original;
        if (code[3] != desired) {
            // IO is /proc/<pid>/mem (not process_vm_writev), one aligned word.
            // Kernel copy_to_user_page handles the remote executable-page cache
            // maintenance on arm64. No local __clear_cache on a foreign address.
            if (!io.write(base + layout->site, desired)) return;
            if (enabled) owned_ = true;
            uint32_t applied = 0;
            if (!io.read(base + layout->site, applied) || applied != desired) return;
        }
        owned_ = enabled;
        status = enabled ? Status::Active : Status::Off;
    }
};
} // namespace fast_loot

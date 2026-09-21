#pragma once
#include "viewmodel.h"
#include <array>
#include <limits>

namespace weapon_tuning {
// Reviewed ARM64 consumers, not guessed field offsets. See WEAPON_TUNING.md.
// Each site replaces ONE instruction; the two spread sites form one feature.
struct Site {
    uint64_t rva;
    std::array<uint32_t, 8> guard; // site-12; changed word is [3]
};
struct Layout {
    std::array<uint8_t, 20> build_id;
    std::array<Site, 3> sites; // recoil accumulator, dynamic spread, base spread
};
inline constexpr Layout kLayouts[] = {
    {{0xd8,0x5e,0x63,0xcd,0x8d,0xcb,0xd4,0x81,0x69,0x50,0x12,0xef,0x2e,0x28,0x4f,0x0e,0xeb,0x9e,0xf3,0x07}, {{
        {0x660c214, {0x2e21fc00,0xfd400121,0x0e20d420,0xfd000120,0x394aa268,0x340008e8,0xf9406668,0xb4001fc8}},
        {0x73425c8, {0x1e212000,0x1e20cc21,0x1e222000,0x1e214c40,0xf84107fe,0xd65f03c0,0x97912f2f,0x1e2e1000}},
        {0x73425f0, {0xb40000a8,0xbd403500,0xbd402001,0x1e210800,0xd65f03c0,0xf81f0ffe,0x97912f25,0xf9400808}},
    }}},
    {{0xe1,0x07,0x95,0x34,0x9b,0xd7,0xc2,0xcd,0xc7,0xd8,0x52,0x75,0x97,0xc6,0x43,0x95,0xa4,0xef,0xed,0x79}, {{
        {0x6565aec, {0x2e21fc00,0xfd400121,0x0e20d420,0xfd000120,0x394ac268,0x340008e8,0xf9406668,0xb4001fc8}},
        {0x72dae70, {0x1e212000,0x1e20cc21,0x1e222000,0x1e214c40,0x6cc42beb,0xd65f03c0,0x9792a387,0x6dbc2beb}},
        {0x72db044, {0xb40000a8,0xbd403500,0xbd402001,0x1e210800,0xd65f03c0,0xf81f0ffe,0x9792a312,0xa9be57fe}},
    }}},
};
inline constexpr uint32_t kNoRecoil = 0xf900013f; // STR XZR,[X9]: both accumulated axes = 0
inline constexpr uint32_t kNoSpread = 0x1e2703e0; // FMOV S0,WZR: exact +0 after ADS/clamp
inline constexpr std::array<uint32_t, 4> kGnuNote = {4,20,3,0x00554e47};
inline bool instruction_supported() {
#if defined(__aarch64__)
    return true;
#else
    return false;
#endif
}
struct Status {
    viewmodel::Status recoil = viewmodel::Status::Off;
    viewmodel::Status spread = viewmodel::Status::Off;
};

template<class IO> class Controller {
    IO& io;
    const bool supported_;
    uint64_t session_ = 0, base_ = 0;
    const Layout* layout_ = nullptr;
    std::array<bool, 3> owned_{};
    using Words = std::array<uint32_t, 8>;
    static uint32_t patched(size_t i) { return i == 0 ? kNoRecoil : kNoSpread; }
    bool owns(size_t first, size_t end) const {
        for (size_t i = first; i < end; ++i) if (owned_[i]) return true;
        return false;
    }
    bool read_site(size_t i, Words& code) {
        return io.read(base_ + layout_->sites[i].rva - 12, code);
    }
    bool context_matches(size_t i, const Words& code) const {
        for (size_t w = 0; w < code.size(); ++w)
            if (w != 3 && code[w] != layout_->sites[i].guard[w]) return false;
        return true;
    }
    // Restore each owned site independently, even if another site is unreadable.
    // Do not adopt someone else's identical patch or overwrite foreign code.
    bool restore_group(size_t first, size_t end) {
        bool ok = true;
        for (size_t i = first; i < end; ++i) {
            if (!owned_[i]) continue;
            Words code{};
            if (!read_site(i, code)) { ok = false; continue; }
            const uint32_t original = layout_->sites[i].guard[3];
            if (code[3] == original) { owned_[i] = false; continue; }
            if (code[3] != patched(i)) { owned_[i] = false; ok = false; continue; }
            if (!context_matches(i, code)) { ok = false; continue; }
            const uint64_t address = base_ + layout_->sites[i].rva;
            // /proc/pid/mem services the remote executable-page cache on arm64.
            const bool written = io.write(address, original);
            uint32_t check = 0;
            if (io.read(address, check) && check == original) owned_[i] = false;
            if (!written || owned_[i]) ok = false;
        }
        return ok;
    }
    viewmodel::Status tick_group(size_t first, size_t end, bool enabled) {
        using S = viewmodel::Status;
        if (!enabled) return restore_group(first, end) ? S::Off : S::Failed;
        std::array<Words, 3> code{};
        bool valid = true;
        // Preflight ALL sites before writing the first half of No Spread.
        for (size_t i = first; i < end; ++i) {
            if (!read_site(i, code[i])) { valid = false; continue; }
            if (!context_matches(i, code[i])) valid = false;
            const auto word = code[i][3];
            if (word != layout_->sites[i].guard[3] && (word != patched(i) || !owned_[i])) {
                owned_[i] = false;
                valid = false;
            }
        }
        if (!valid) { restore_group(first, end); return S::Failed; }
        for (size_t i = first; i < end; ++i) {
            if (code[i][3] == patched(i)) continue;
            const uint64_t address = base_ + layout_->sites[i].rva;
            // Keep an ambiguous attempt pending until readback/restore confirms
            // the original. A failed write call need not mean zero bytes changed.
            owned_[i] = true;
            const bool written = io.write(address, patched(i));
            uint32_t check = 0;
            if (!written || !io.read(address, check) || check != patched(i)) {
                restore_group(first, end);
                return S::Failed;
            }
        }
        return S::Active;
    }
public:
    Status status{};
    explicit Controller(IO& memory, bool supported) : io(memory), supported_(supported) {}
    bool has_state() const { return owns(0, 3); }
    bool reset(bool restore) {
        if (restore && has_state()) tick(session_, base_, false, false);
        const bool ok = !restore || !has_state();
        session_ = base_ = 0; layout_ = nullptr; owned_ = {}; status = {};
        return ok;
    }
    void tick(uint64_t session, uint64_t base, bool recoil, bool spread) {
        using S = viewmodel::Status;
        if (session != session_ || base != base_) {
            // A lost/replaced process must never receive writes to old addresses.
            reset(false); session_ = session; base_ = base;
        }
        status = {recoil ? S::Waiting : S::Off, spread ? S::Waiting : S::Off};
        if (!recoil && !spread && !has_state()) return;
        if (!base) return;
        status = {(recoil || owns(0,1)) ? S::Failed : S::Off,
                  (spread || owns(1,3)) ? S::Failed : S::Off};
        if ((base & 3u) || base > std::numeric_limits<uint64_t>::max() - 0x8000000u) return;
        if ((recoil || spread) && !supported_) return;
        std::array<uint32_t, 4> note{};
        std::array<uint8_t, 20> id{};
        if (!io.read(base + 0x308, note) || note != kGnuNote || !io.read(base + 0x318, id)) return;
        const Layout* found = nullptr;
        for (const auto& candidate : kLayouts) if (id == candidate.build_id) found = &candidate;
        if (!found || (has_state() && layout_ != found)) return;
        layout_ = found;
        status.recoil = tick_group(0, 1, recoil);
        status.spread = tick_group(1, 3, spread);
    }
};
} // namespace weapon_tuning

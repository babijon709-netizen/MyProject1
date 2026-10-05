#pragma once
// Client hit-area remap for the verified KCC HitBox list.  It never changes
// library code: only a live managed HitBox.m_HitArea is written through IO's
// writable-heap gate, and every changed value is restored on release.
#include "game_offsets_active.h"
#include "viewmodel.h"

#include <cstdint>
#include <vector>

namespace headshot_hitbox {

inline constexpr std::size_t kMaxHitBoxes = 64;
inline constexpr std::size_t kMaxTracked = 512;

template<class IO> bool pointer(IO& io, uint64_t address, uint64_t& value) {
    if (!io.read(address, value)) return false;
    value &= 0x00ffffffffffffffULL;
    return true;
}

struct Size3 { float x, y, z; };
static_assert(sizeof(Size3) == 12, "HitBox size layout");

struct Identity {
    uint64_t object = 0, klass = 0, native = 0;

    template<class IO> bool capture(IO& io, uint64_t object_) {
        object = object_;
        return object >= 0x10000 && pointer(io, object, klass) && klass >= 0x10000 &&
            pointer(io, object + go_active::MANAGED_CACHED_PTR, native) && native >= 0x10000;
    }
    template<class IO> bool live(IO& io) const {
        uint64_t current = 0;
        return object >= 0x10000 && pointer(io, object, current) && current == klass &&
            pointer(io, object + go_active::MANAGED_CACHED_PTR, current) && current == native;
    }
};

template<class IO> class Controller {
    struct Saved {
        Identity identity{};
        int32_t original_area = 0;
        uint64_t seen_frame = 0;
    };

    IO& io_;
    std::vector<Saved> saved_;
    uint64_t frame_ = 0;

    typename std::vector<Saved>::iterator found(uint64_t object) {
        for (auto it = saved_.begin(); it != saved_.end(); ++it)
            if (it->identity.object == object) return it;
        return saved_.end();
    }
    bool restore(Saved& entry) {
        if (!entry.identity.live(io_)) return true; // object was recycled/destroyed
        int32_t current = -1;
        if (!io_.read(entry.identity.object + go_active::HITBOX_AREA, current)) return false;
        // A scene/other writer changed the slot after us. It no longer belongs
        // to this controller, so leave that newer value untouched.
        if (current != 0) return true;
        if (!io_.write_heap(entry.identity.object + go_active::HITBOX_AREA,
                            entry.original_area)) return false;
        return io_.read(entry.identity.object + go_active::HITBOX_AREA, current) &&
               current == entry.original_area;
    }

public:
    explicit Controller(IO& io) : io_(io) {}
    std::size_t tracked() const { return saved_.size(); }

    // One ESP snapshot is one frame. Entries not observed in that snapshot are
    // immediately restored, so a despawned/removed player cannot leave a stale
    // area override behind.
    void begin_frame(bool enabled) {
        ++frame_;
        if (!enabled) reset(true);
    }
    void end_frame() {
        for (auto it = saved_.begin(); it != saved_.end();) {
            if (it->seen_frame == frame_) { ++it; continue; }
            // Keep a live entry on a transient read/write failure. The next
            // frame retries restoration instead of forgetting an owned value.
            if (restore(*it)) it = saved_.erase(it);
            else ++it;
        }
    }
    void reset(bool restore_original) {
        if (!restore_original) { saved_.clear(); return; }
        for (auto it = saved_.begin(); it != saved_.end();) {
            // As with end_frame, a failed restore remains pending for a later
            // tick; a gone object is safe to forget.
            if (restore(*it)) it = saved_.erase(it);
            else ++it;
        }
    }

    // KCC.hitBoxRecorderRoot -> HitBox[] is game-owned and is already tied to
    // a validated remote PlayerManager by the caller.  Areas 1..4 are the body
    // hit volumes; they are temporarily mapped to area 0 (Head).  Existing
    // head volumes are neither written nor tracked.
    bool apply_kcc(uint64_t kcc) {
        uint64_t root = 0, array = 0;
        int32_t count = 0;
        if (kcc < 0x10000 ||
            !pointer(io_, kcc + go_active::KCC_HITBOX_ROOT, root) || !root ||
            !pointer(io_, root + go_active::HITBOX_ROOT_ARRAY, array) || !array ||
            !io_.read(array + go_active::IL2CPP_ARRAY_LENGTH, count) ||
            count <= 0 || (std::size_t)count > kMaxHitBoxes)
            return false;

        bool changed = false;
        for (int32_t i = 0; i < count; ++i) {
            uint64_t hitbox = 0;
            if (!pointer(io_, array + go_active::IL2CPP_ARRAY_FIRST_ELEMENT +
                              (uint64_t)i * sizeof(uint64_t), hitbox) || !hitbox)
                continue;
            int32_t area = -1;
            Size3 size{};
            if (!io_.read(hitbox + go_active::HITBOX_AREA, area) || area < 0 || area > 4 ||
                !io_.read(hitbox + go_active::HITBOX_SIZE, size))
                continue;
            if (!viewmodel::finite(size.x) || !viewmodel::finite(size.y) || !viewmodel::finite(size.z) ||
                size.x <= 0.02f || size.y <= 0.02f || size.z <= 0.02f ||
                size.x > 3.f || size.y > 3.f || size.z > 3.f)
                continue;

            auto saved = found(hitbox);
            if (saved != saved_.end() && !saved->identity.live(io_)) {
                saved_.erase(saved);
                saved = saved_.end();
            }
            if (area == 0) {
                if (saved != saved_.end()) saved->seen_frame = frame_;
                continue;
            }
            if (saved == saved_.end()) {
                if (saved_.size() >= kMaxTracked) continue;
                Identity identity{};
                if (!identity.capture(io_, hitbox)) continue;
                if (!io_.write_heap(hitbox + go_active::HITBOX_AREA, int32_t{0})) continue;
                // Record ownership immediately after a successful write. If
                // readback itself is transiently unavailable, reset/end_frame
                // can still restore the original later instead of leaking 0.
                saved_.push_back({identity, area, frame_});
                int32_t checked = -1;
                if (!io_.read(hitbox + go_active::HITBOX_AREA, checked) || checked != 0) continue;
                changed = true;
            } else {
                saved->seen_frame = frame_;
                if (!io_.write_heap(hitbox + go_active::HITBOX_AREA, int32_t{0})) continue;
                int32_t checked = -1;
                if (!io_.read(hitbox + go_active::HITBOX_AREA, checked) || checked != 0) continue;
                changed = true;
            }
        }
        return changed;
    }
};
} // namespace headshot_hitbox

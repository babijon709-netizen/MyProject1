#pragma once

#include <cstdint>

namespace skeletonframehold {

// World-space bones stay usable briefly when a hierarchy read lands while
// Unity is rebuilding its animation buffers. They are projected again each
// frame, so the overlay follows the current camera rather than freezing.
constexpr std::uint8_t kMaxWorldAge = 30;
constexpr int kRebuildAfterFailures = 90;

inline bool usable(std::uint8_t age) {
    return age >= 1 && age <= kMaxWorldAge;
}

inline void refresh(std::uint8_t& age) {
    age = 1;
}

inline bool reuse(std::uint8_t& age) {
    if (!usable(age) || age == kMaxWorldAge) return false;
    ++age;
    return true;
}

}  // namespace skeletonframehold

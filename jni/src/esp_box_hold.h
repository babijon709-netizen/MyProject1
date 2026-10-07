#pragma once
// A healthy frame is authoritative. A transient frame merges readable entries
// by stable id and expires missing ones after a short time window.
#include <algorithm>
#include <cstddef>
#include <vector>
#include "game.h"

namespace espboxhold {

template <typename Value>
struct Entry {
    Value value{};
    double last_seen = 0.0;
};

template <typename Value>
inline void update(std::vector<Entry<Value>>& hold,
                   const std::vector<Value>& fresh,
                   bool transient,
                   double now,
                   double hold_seconds,
                   std::vector<Value>& out) {
    if (!transient) {
        hold.clear();
        hold.reserve(fresh.size());
        for (const Value& value : fresh) {
            Entry<Value> entry;
            entry.value = value;
            entry.last_seen = now;
            hold.push_back(entry);
        }
    } else {
        for (const Value& value : fresh) {
            auto found = std::find_if(hold.begin(), hold.end(), [&](const Entry<Value>& entry) {
                return entry.value.id == value.id;
            });
            if (found != hold.end()) {
                found->value = value;
                found->last_seen = now;
            } else {
                Entry<Value> entry;
                entry.value = value;
                entry.last_seen = now;
                hold.push_back(entry);
            }
        }
        for (std::size_t index = 0; index < hold.size();) {
            if (now - hold[index].last_seen > hold_seconds)
                hold.erase(hold.begin() + (std::ptrdiff_t)index);
            else
                ++index;
        }
    }

    out.clear();
    out.reserve(hold.size());
    for (const Entry<Value>& entry : hold) out.push_back(entry.value);
}

}  // namespace espboxhold

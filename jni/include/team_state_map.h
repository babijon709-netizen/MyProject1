#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Safe, host-testable reader for the game's TeammateStates dictionary
// (Dictionary<string, TeammateStatus>). Memory access and managed-string
// decoding are supplied by callbacks so this layout logic can be tested
// without attaching to the game process.
namespace team_state_map {

constexpr std::size_t kMaxDictionaryEntries = 256;
constexpr std::size_t kUserIdCapacity = 40;

// Returns false for incomplete/invalid snapshots. On success, out_ids contains
// the active string keys only; enum values are intentionally ignored because
// Alive/Dead/Offline all identify the same teammate.
template <typename ReadPointer, typename ReadInt32, typename ReadString,
          typename IsValidObject>
bool read_user_ids(std::uint64_t dictionary,
                   std::uint64_t entries_offset,
                   std::uint64_t count_offset,
                   std::uint64_t array_length_offset,
                   std::uint64_t array_items_offset,
                   std::uint64_t entry_stride,
                   std::uint64_t entry_key_offset,
                   ReadPointer&& read_pointer,
                   ReadInt32&& read_int32,
                   ReadString&& read_string,
                   IsValidObject&& is_valid_object,
                   char (*out_ids)[kUserIdCapacity],
                   std::size_t out_capacity,
                   std::size_t& out_count,
                   std::int32_t* raw_entry_count = nullptr) {
    out_count = 0;
    if (raw_entry_count) *raw_entry_count = -1;
    if (!is_valid_object(dictionary) || !out_ids || out_capacity == 0 ||
        entry_stride == 0 || entry_key_offset >= entry_stride) return false;

    std::uint64_t entries = 0;
    std::int32_t count = 0;
    if (!read_pointer(dictionary + entries_offset, entries) ||
        !read_int32(dictionary + count_offset, count) ||
        count < 0 || static_cast<std::size_t>(count) > kMaxDictionaryEntries) return false;
    if (raw_entry_count) *raw_entry_count = count;
    if (count == 0) return true;
    if (!is_valid_object(entries)) return false;

    std::int32_t array_length = 0;
    if (!read_int32(entries + array_length_offset, array_length) ||
        array_length < count || array_length < 0) return false;

    const std::uint64_t first = entries + array_items_offset;
    std::size_t found = 0;
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uint64_t entry = first + static_cast<std::uint64_t>(i) * entry_stride;
        std::int32_t hash = -1;
        if (!read_int32(entry, hash)) return false;
        if (hash < 0) continue; // unused slot or a removed dictionary entry

        std::uint64_t key = 0;
        if (!read_pointer(entry + entry_key_offset, key) || !is_valid_object(key))
            return false;
        char user_id[kUserIdCapacity] = {};
        if (!read_string(key, user_id, sizeof(user_id),
                         static_cast<int>(sizeof(user_id) - 1)) || !user_id[0])
            return false;

        bool duplicate = false;
        for (std::size_t j = 0; j < found; ++j) {
            if (std::strcmp(out_ids[j], user_id) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        // Reject rather than publish a truncated set: omitting a teammate would
        // make the aim filter incomplete and could select that teammate.
        if (found >= out_capacity) return false;
        std::memcpy(out_ids[found], user_id, sizeof(user_id));
        ++found;
    }

    out_count = found;
    return true;
}

} // namespace team_state_map

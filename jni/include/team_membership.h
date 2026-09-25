#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Pure team/ESP predicates shared by the memory reader and overlay. Keeping
// these rules independent of process-memory access makes the safety boundary
// (teammates are never aim targets; their ESP is controlled by one toggle)
// straightforward to test on the host.
namespace team_membership {

struct PlayerGroup {
    char team[40] = {};
    char clan[48] = {};
    char tag[16] = {};

    bool any() const { return team[0] || clan[0] || tag[0]; }
};

inline bool same_nonempty(const char* a, const char* b) {
    return a && b && a[0] && b[0] && std::strcmp(a, b) == 0;
}

// Return count when the authoritative PlayerManager pointer is not present.
// Camera proximity is deliberately not a substitute for this exact identity.
inline std::size_t find_player_index_by_address(const std::uint64_t* players,
                                                std::size_t count,
                                                std::uint64_t wanted) {
    if (!players || wanted == 0) return count;
    for (std::size_t i = 0; i < count; ++i)
        if (players[i] == wanted) return i;
    return count;
}

inline bool groups_are_allied(const PlayerGroup& local, const PlayerGroup& other) {
    // Empty values must never match each other: failed reads are not proof that
    // two players share a team or clan. Clan tags are a useful fallback on game
    // builds/modes where the clan ID is not replicated to PlayerManager. If
    // both IDs exist and differ, the tag alone is not enough to override that.
    if (same_nonempty(local.team, other.team) ||
        same_nonempty(local.clan, other.clan)) return true;
    return (!local.clan[0] || !other.clan[0]) &&
           same_nonempty(local.tag, other.tag);
}

inline bool roster_entry_matches(const char* user_id, const char* display_name,
                                 const char* member_id, const char* member_nick,
                                 std::uint32_t player_net_id = 0,
                                 std::uint32_t member_net_id = 0) {
    // Prefer the stable user ID, while retaining exact nickname and Mirror
    // NetworkIdentity.netId fallbacks for pooled PlayerManager objects or when
    // one of the managed strings is unavailable. Zero is not a usable netId.
    return same_nonempty(user_id, member_id) ||
           same_nonempty(display_name, member_nick) ||
           (player_net_id != 0 && member_net_id != 0 &&
            player_net_id == member_net_id);
}

inline bool include_in_esp(bool ally, bool show_allies) {
    return !ally || show_allies;
}

} // namespace team_membership

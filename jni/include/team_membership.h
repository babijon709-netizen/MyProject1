#pragma once

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

inline bool groups_are_allied(const PlayerGroup& local, const PlayerGroup& other) {
    // Empty values must never match each other: failed reads are not proof that
    // two players share a team or clan.
    return same_nonempty(local.team, other.team) ||
           same_nonempty(local.clan, other.clan);
}

inline bool roster_entry_matches(const char* user_id, const char* display_name,
                                 const char* member_id, const char* member_nick) {
    // Prefer the stable user ID, while retaining an exact nickname match as a
    // backup for transient/missing ID reads or a pooled PlayerManager cache.
    return same_nonempty(user_id, member_id) ||
           same_nonempty(display_name, member_nick);
}

inline bool include_in_esp(bool ally, bool show_allies) {
    return !ally || show_allies;
}

} // namespace team_membership

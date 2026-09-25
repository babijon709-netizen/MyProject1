#include "team_membership.h"
#include "team_state_map.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int failures = 0;

struct FakeRemoteMemory {
    static constexpr std::size_t kSize = 1024;
    alignas(16) unsigned char bytes[kSize]{};

    std::uint64_t base() const {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(bytes));
    }
    bool valid(std::uint64_t address) const {
        return address >= base() && address < base() + kSize && (address & 7u) == 0;
    }
    bool read(std::uint64_t address, void* out, std::size_t size) const {
        if (!out || address < base() || size > kSize ||
            address - base() > kSize - size) return false;
        std::memcpy(out, bytes + (address - base()), size);
        return true;
    }
    bool write(std::uint64_t address, const void* value, std::size_t size) {
        if (!value || address < base() || size > kSize ||
            address - base() > kSize - size) return false;
        std::memcpy(bytes + (address - base()), value, size);
        return true;
    }
    template <typename T>
    bool put(std::uint64_t address, const T& value) {
        return write(address, &value, sizeof(value));
    }
    bool put_string(std::uint64_t address, const char* value) {
        return write(address, value, std::strlen(value) + 1);
    }
};

static bool test_teammate_state_dictionary() {
    FakeRemoteMemory memory;
    const std::uint64_t dict = memory.base() + 0x40;
    const std::uint64_t entries = memory.base() + 0x100;
    const std::uint64_t id_a = memory.base() + 0x300;
    const std::uint64_t id_b = memory.base() + 0x320;
    const std::uint64_t id_duplicate = memory.base() + 0x340;
    const std::uint64_t entry_base = entries + 0x20;

    const std::uint64_t stored_entries = entries;
    const std::int32_t count = 4;
    const std::int32_t array_length = 4;
    const std::int32_t hash_a = 17;
    const std::int32_t removed_hash = -1;
    const std::int32_t hash_b = 21;
    const std::int32_t hash_duplicate = 28;
    bool ok = memory.put(dict + 0x18, stored_entries) &&
              memory.put(dict + 0x20, count) &&
              memory.put(entries + 0x18, array_length) &&
              memory.put(entry_base + 0x00, hash_a) &&
              memory.put(entry_base + 0x08, id_a) &&
              memory.put(entry_base + 0x18, removed_hash) &&
              memory.put(entry_base + 0x30, hash_b) &&
              memory.put(entry_base + 0x38, id_b) &&
              memory.put(entry_base + 0x48, hash_duplicate) &&
              memory.put(entry_base + 0x50, id_duplicate) &&
              memory.put_string(id_a, "user-a") &&
              memory.put_string(id_b, "user-b") &&
              memory.put_string(id_duplicate, "user-a");
    if (!ok) return false;

    auto read_pointer = [&](std::uint64_t address, std::uint64_t& value) {
        return memory.read(address, &value, sizeof(value));
    };
    auto read_int32 = [&](std::uint64_t address, std::int32_t& value) {
        return memory.read(address, &value, sizeof(value));
    };
    auto read_string = [&](std::uint64_t address, char* out, std::size_t cap,
                           int max_chars) {
        if (!out || cap == 0 || max_chars < 0) return false;
        for (int i = 0; i <= max_chars && static_cast<std::size_t>(i) < cap; ++i) {
            char ch = '\0';
            if (!memory.read(address + static_cast<std::uint64_t>(i), &ch, 1)) return false;
            out[i] = ch;
            if (ch == '\0') return true;
        }
        out[cap - 1] = '\0';
        return false;
    };
    auto valid_object = [&](std::uint64_t address) { return memory.valid(address); };

    char ids[4][team_state_map::kUserIdCapacity]{};
    std::size_t found = 0;
    std::int32_t raw_count = -1;
    const bool parsed = team_state_map::read_user_ids(
        dict, 0x18, 0x20, 0x18, 0x20, 0x18, 0x08,
        read_pointer, read_int32, read_string, valid_object,
        ids, 4, found, &raw_count);
    const bool unique_ids_ok = parsed && raw_count == 4 && found == 2 &&
        std::strcmp(ids[0], "user-a") == 0 &&
        std::strcmp(ids[1], "user-b") == 0 &&
        team_membership::roster_entry_matches("user-b", "", ids[1], "");

    // One unreadable active key must reject the whole snapshot, not publish
    // the first ID and silently omit later teammates.
    const std::uint64_t invalid_key = memory.base() + 0x8000;
    const bool changed = memory.put(entry_base + 0x08, invalid_key);
    std::size_t partial_count = 99;
    const bool rejected_partial = changed && !team_state_map::read_user_ids(
        dict, 0x18, 0x20, 0x18, 0x20, 0x18, 0x08,
        read_pointer, read_int32, read_string, valid_object,
        ids, 4, partial_count, nullptr) && partial_count == 0;

    // A confirmed empty Dictionary is valid even with a null entries array.
    const std::uint64_t null_entries = 0;
    const std::int32_t empty_count = 0;
    memory.put(dict + 0x18, null_entries);
    memory.put(dict + 0x20, empty_count);
    std::size_t empty_count_out = 99;
    const bool empty_ok = team_state_map::read_user_ids(
        dict, 0x18, 0x20, 0x18, 0x20, 0x18, 0x08,
        read_pointer, read_int32, read_string, valid_object,
        ids, 4, empty_count_out, nullptr) && empty_count_out == 0;
    return unique_ids_ok && rejected_partial && empty_ok;
}

static void check(bool ok, const char* description) {
    std::printf("  %-62s %s\n", description, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

int main() {
    using team_membership::PlayerGroup;

    const std::uint64_t players[] = {0x1000, 0x2000, 0x3000};
    check(team_membership::find_player_index_by_address(players, 3, 0x2000) == 1,
          "local PlayerManager address wins over camera-nearest candidates");
    check(team_membership::find_player_index_by_address(players, 3, 0x9000) == 3,
          "missing authoritative player address is reported as not found");

    PlayerGroup local{};
    PlayerGroup remote{};
    check(!team_membership::groups_are_allied(local, remote),
          "empty/failed group reads do not classify everyone as an ally");

    std::snprintf(local.team, sizeof(local.team), "%s", "party-42");
    std::snprintf(remote.team, sizeof(remote.team), "%s", "party-42");
    check(team_membership::groups_are_allied(local, remote),
          "same non-empty team is an ally");

    remote.team[0] = '\0';
    std::snprintf(local.clan, sizeof(local.clan), "%s", "clan-7");
    std::snprintf(remote.clan, sizeof(remote.clan), "%s", "clan-7");
    check(team_membership::groups_are_allied(local, remote),
          "same non-empty clan is an ally even when team data is missing");

    remote.clan[0] = '\0';
    check(!team_membership::groups_are_allied(local, remote),
          "a missing remote group does not match the local group");

    std::snprintf(local.clan, sizeof(local.clan), "%s", "clan-7");
    std::snprintf(local.tag, sizeof(local.tag), "%s", "WOLF");
    std::snprintf(remote.tag, sizeof(remote.tag), "%s", "WOLF");
    check(team_membership::groups_are_allied(local, remote),
          "matching clan tags recover allies when one clan ID is missing");
    std::snprintf(remote.clan, sizeof(remote.clan), "%s", "clan-8");
    check(!team_membership::groups_are_allied(local, remote),
          "different known clan IDs override a coincidentally matching tag");
    remote.clan[0] = '\0';
    remote.tag[0] = '\0';
    local.tag[0] = '\0';

    check(team_membership::roster_entry_matches("user-17", "Nick", "user-17", "Other"),
          "team roster matches the stable user ID");
    check(team_membership::roster_entry_matches("", "Nick", "", "Nick"),
          "team roster falls back to the visible nickname when IDs are unavailable");
    check(team_membership::roster_entry_matches("", "", "", "", 733, 733),
          "team roster matches the Mirror network ID when strings are unavailable");
    check(!team_membership::roster_entry_matches("", "", "", "", 733, 734),
          "different network IDs do not match");
    check(!team_membership::roster_entry_matches("", "", "", "", 0, 0),
          "zero network IDs are not treated as valid matches");
    check(!team_membership::roster_entry_matches("", "", "", ""),
          "empty roster fields do not match");
    check(test_teammate_state_dictionary(),
          "TeammateStates reader deduplicates IDs and rejects partial snapshots");

    check(!team_membership::include_in_esp(true, false),
          "ally ESP off hides teammates");
    check(team_membership::include_in_esp(true, true),
          "ally ESP on includes teammates in enabled player visuals");
    check(team_membership::include_in_esp(false, false),
          "ally ESP toggle does not hide enemies");

    if (failures) {
        std::printf("%d team-membership check(s) failed\n", failures);
        return 1;
    }
    std::puts("all team-membership checks passed");
    return 0;
}

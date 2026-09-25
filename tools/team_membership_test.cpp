#include "team_membership.h"

#include <cstdio>

static int failures = 0;

static void check(bool ok, const char* description) {
    std::printf("  %-62s %s\n", description, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

int main() {
    using team_membership::PlayerGroup;

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

    check(team_membership::roster_entry_matches("user-17", "Nick", "user-17", "Other"),
          "team roster matches the stable user ID");
    check(team_membership::roster_entry_matches("", "Nick", "", "Nick"),
          "team roster falls back to the visible nickname when IDs are unavailable");
    check(!team_membership::roster_entry_matches("", "", "", ""),
          "empty roster fields do not match");

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

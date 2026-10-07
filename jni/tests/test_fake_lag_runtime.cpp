#include "fake_lag.h"
#include "logfile.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace {
std::string test_dir;
std::string original_path;

constexpr const char* kSuScript = R"SH(#!/bin/sh
[ "$1" = "-c" ] || exit 64
exec /bin/sh -c "$2"
)SH";

constexpr const char* kIptablesScript = R"PY(#!/usr/bin/env python3
import json, os, sys
path = os.environ["FAKE_IPTABLES_STATE"]
with open(path) as stream:
    state = json.load(stream)
args = sys.argv[1:]
if args[:2] == ["-w", "2"]:
    args = args[2:]
chain = "XVCEN_FL"
legacy = ["-C", "INPUT", "!", "-i", "lo", "-p", "udp", "-m", "length", "--length", "80:65535", "-j", "DROP"]
legacy_delete = ["-D", "INPUT"] + legacy[2:]
chain_filter = ["-A", chain, "!", "-i", "lo", "-p", "udp", "-m", "length", "--length", "80:65535", "-j", "DROP"]
changed = False
result = 2
if args == ["-C", "INPUT", "-j", chain]:
    result = 0 if state["jump"] else 1
elif args == ["-S", chain]:
    result = 0 if state["chain"] else 1
elif args == legacy:
    result = 0 if state["legacy"] else 1
elif args == ["-D", "INPUT", "-j", chain]:
    if state.get("fail_remove", 0) > 0:
        state["fail_remove"] -= 1
        changed, result = True, 2
    elif state["jump"]:
        state["jump"] = False
        changed, result = True, 0
    else:
        result = 1
elif args == ["-D"] + legacy_delete[1:]:
    if state["legacy"]:
        state["legacy"] = False
        changed, result = True, 0
    else:
        result = 1
elif args == ["-N", chain]:
    if not state["chain"]:
        state["chain"] = True
        changed, result = True, 0
    else:
        result = 1
elif args == chain_filter:
    if state["chain"]:
        state["filter"] = True
        changed, result = True, 0
elif args == ["-I", "INPUT", "1", "-j", chain]:
    if state["chain"] and state["filter"]:
        state["jump"] = True
        changed, result = True, 0
elif args == ["-F", chain]:
    if state["chain"]:
        state["filter"] = False
        changed, result = True, 0
    else:
        result = 1
elif args == ["-X", chain]:
    if state["chain"] and not state["jump"] and not state["filter"]:
        state["chain"] = False
        changed, result = True, 0
    else:
        result = 1
if changed:
    temporary = path + ".tmp"
    with open(temporary, "w") as stream:
        json.dump(state, stream, separators=(",", ":"))
    os.replace(temporary, path)
sys.exit(result)
)PY";

void cleanup_test_environment() {
    if (!original_path.empty()) setenv("PATH", original_path.c_str(), 1);
    unsetenv("FAKE_IPTABLES_STATE");
    if (!test_dir.empty()) {
        unlink((test_dir + "/su").c_str());
        unlink((test_dir + "/iptables").c_str());
        unlink((test_dir + "/rules.json.tmp").c_str());
        unlink((test_dir + "/rules.json").c_str());
        rmdir(test_dir.c_str());
    }
}

bool write_text(const std::string& path, const char* text) {
    std::ofstream output(path);
    output << text;
    return output.good();
}

bool read_rule_state(const std::string& path, bool& chain, bool& filter,
                     bool& jump, bool& legacy) {
    std::ifstream input(path);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    if (!input.good() && !input.eof()) return false;
    auto field = [&](const char* name) {
        return text.find(std::string("\"") + name + "\":true") != std::string::npos;
    };
    chain = field("chain");
    filter = field("filter");
    jump = field("jump");
    legacy = field("legacy");
    return true;
}

bool wait_for_rule_state(const std::string& path, bool want_blocked) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        bool chain = false, filter = false, jump = false, legacy = false;
        if (read_rule_state(path, chain, filter, jump, legacy) &&
            chain == want_blocked && filter == want_blocked &&
            jump == want_blocked && !legacy)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool wait_for_active_rule(const std::string& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        bool chain = false, filter = false, jump = false, legacy = false;
        if (read_rule_state(path, chain, filter, jump, legacy) &&
            chain && filter && jump && !legacy &&
            fake_lag::status() == fake_lag::Status::Active)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool retry_until_unblocked(const std::string& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        fake_lag::update(false, 100);
        bool chain = false, filter = false, jump = false, legacy = false;
        if (read_rule_state(path, chain, filter, jump, legacy) &&
            !chain && !filter && !jump && !legacy &&
            fake_lag::status() == fake_lag::Status::Off)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
} // namespace

void LogLine(const char*, ...) {}

int main() {
    char template_path[] = "/tmp/fake-lag-test-XXXXXX";
    char* created = mkdtemp(template_path);
    if (!created) return 1;
    test_dir = created;
    original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    if (std::atexit(cleanup_test_environment) != 0) return 1;

    const std::string state_path = test_dir + "/rules.json";
    const std::string su_path = test_dir + "/su";
    const std::string iptables_path = test_dir + "/iptables";
    if (!write_text(su_path, kSuScript) || !write_text(iptables_path, kIptablesScript) ||
        chmod(su_path.c_str(), 0700) != 0 || chmod(iptables_path.c_str(), 0700) != 0 ||
        !write_text(state_path, "{\"chain\":false,\"filter\":false,\"jump\":false,\"legacy\":true,\"fail_remove\":0}"))
        return 1;

    const std::string path = test_dir + ":" + original_path;
    if (setenv("PATH", path.c_str(), 1) != 0 ||
        setenv("FAKE_IPTABLES_STATE", state_path.c_str(), 1) != 0)
        return 1;

    fake_lag::update(false, 100); // startup also removes the Drive-era direct rule
    if (!wait_for_rule_state(state_path, false) || fake_lag::status() != fake_lag::Status::Off) {
        std::puts("FAIL: startup cleanup");
        fake_lag::shutdown();
        return 1;
    }

    fake_lag::update(true, 100); // the permanent-block setting is the critical cleanup path
    if (!wait_for_active_rule(state_path)) {
        std::puts("FAIL: incoming UDP rule was not installed");
        fake_lag::shutdown();
        return 1;
    }

    // Simulate a transient iptables failure during removal. The worker and
    // subsequent disabled updates must retry until the chain is actually gone.
    if (!write_text(state_path,
                    "{\"chain\":true,\"filter\":true,\"jump\":true,\"legacy\":false,\"fail_remove\":2}")) {
        fake_lag::shutdown();
        return 1;
    }
    fake_lag::update(false, 100);
    if (!retry_until_unblocked(state_path)) {
        std::puts("FAIL: disabling left an incoming UDP rule installed");
        fake_lag::shutdown();
        return 1;
    }

    fake_lag::shutdown();
    std::puts("ALL FAKE-LAG RUNTIME TESTS PASS");
    return 0;
}

// Host test for the fast-loot PlayerLoop traversal and recovery paths.
#include "loot_memory.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace {
struct FakeIO {
    std::map<uint64_t, unsigned char> mem;
    bool allow_write = true;

    template<class T> bool read(uint64_t address, T& value) {
        std::memset(&value, 0, sizeof(value));
        for (size_t i = 0; i < sizeof(value); ++i) {
            const auto it = mem.find(address + i);
            if (it == mem.end()) return false;
            reinterpret_cast<unsigned char*>(&value)[i] = it->second;
        }
        return true;
    }
    template<class T> void seed(uint64_t address, const T& value) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) mem[address + i] = bytes[i];
    }
    void seed_text(uint64_t address, const char* text) {
        for (size_t i = 0; ; ++i) {
            mem[address + i] = static_cast<unsigned char>(text[i]);
            if (!text[i]) return;
        }
    }
    template<class T> bool write_heap(uint64_t address, const T& value) {
        if (!allow_write) return false;
        seed(address, value);
        return true;
    }
};

constexpr uint64_t kBase = 0x100000;
constexpr uint64_t kHelperClass = 0x200000;
constexpr uint64_t kRunnerClass = 0x210000;
constexpr uint64_t kDelayClass = 0x220000;
constexpr uint64_t kNameHelper = 0x300000;
constexpr uint64_t kNsHelper = 0x301000;
constexpr uint64_t kNameRunner = 0x302000;
constexpr uint64_t kNsRunner = 0x303000;
constexpr uint64_t kNameDelay = 0x304000;
constexpr uint64_t kNsEmpty = 0x305000;
constexpr uint64_t kStatics1 = 0x400000;
constexpr uint64_t kRunners1 = 0x401000;
constexpr uint64_t kRunner1 = 0x402000;
constexpr uint64_t kItems1 = 0x403000;
constexpr uint64_t kPromise1 = 0x404000;
constexpr uint64_t kStatics2 = 0x410000;
constexpr uint64_t kRunners2 = 0x411000;
constexpr uint64_t kRunner2 = 0x412000;
constexpr uint64_t kItems2 = 0x413000;
constexpr uint64_t kPromise2 = 0x414000;

int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::printf("FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

void seed_class(FakeIO& io, uint64_t klass, uint64_t name, uint64_t ns,
                const char* name_text, const char* ns_text) {
    io.seed(klass + go_active::IL2CPP_CLASS_NAME, name);
    io.seed(klass + go_active::IL2CPP_CLASS_NAMESPACE, ns);
    io.seed_text(name, name_text);
    io.seed_text(ns, ns_text);
}

void seed_promise(FakeIO& io, uint64_t promise, float cadence = 0.35f) {
    io.seed(promise, kDelayClass);
    io.seed(promise + go_active::DELAYPROMISE_TIMESPAN, cadence);
    const float elapsed = 0.10f;
    const int32_t frame = 12345;
    io.seed(promise + go_active::DELAYPROMISE_ELAPSED, elapsed);
    io.seed(promise + go_active::DELAYPROMISE_INITIAL_FRAME, frame);
}

void seed_chain(FakeIO& io, uint64_t statics, uint64_t runners,
                uint64_t runner, uint64_t items, uint64_t promise) {
    io.seed(kHelperClass + go_active::IL2CPP_CLASS_STATIC_FIELDS_OFFSET, statics);
    io.seed(statics + go_active::PLAYERLOOPHELPER_RUNNERS, runners);
    const int32_t one = 1;
    io.seed(runners + go_active::IL2CPP_ARRAY_LENGTH, one);
    io.seed(runners + go_active::IL2CPP_ARRAY_FIRST_ELEMENT, runner);
    io.seed(runner, kRunnerClass);
    io.seed(runner + go_active::LOOPRUNNER_ITEMS, items);
    io.seed(runner + go_active::LOOPRUNNER_TAIL, one);
    io.seed(items + go_active::IL2CPP_ARRAY_LENGTH, one);
    io.seed(items + go_active::IL2CPP_ARRAY_FIRST_ELEMENT, promise);
}

void seed_scene(FakeIO& io) {
    io.seed(kBase + go_active::PLAYER_LOOP_HELPER_TYPEINFO_RVA, kHelperClass);
    seed_class(io, kHelperClass, kNameHelper, kNsHelper,
               "PlayerLoopHelper", "Cysharp.Threading.Tasks");
    seed_class(io, kRunnerClass, kNameRunner, kNsRunner,
               "PlayerLoopRunner", "Cysharp.Threading.Tasks.Internal");
    seed_class(io, kDelayClass, kNameDelay, kNsEmpty,
               "DelayIgnoreTimeScalePromise", "");
    seed_chain(io, kStatics1, kRunners1, kRunner1, kItems1, kPromise1);
    seed_promise(io, kPromise1);
}

float cadence(FakeIO& io, uint64_t promise) {
    float value = -1.f;
    io.read(promise + go_active::DELAYPROMISE_TIMESPAN, value);
    return value;
}
} // namespace

int main() {
    FakeIO io;
    seed_scene(io);
    fast_loot::Controller<FakeIO> controller(io);

    // The exact 0.35-second pickup promise is found and neutralized.
    controller.tick(1, kBase, true);
    CHECK(controller.status == viewmodel::Status::Active, "initial chain -> Active");
    CHECK(cadence(io, kPromise1) == 0.f, "initial promise neutralized");
    CHECK(controller.neutralized() == 1u, "initial write counted");

    // New waits reuse the same class. Cached class identities must not prevent
    // subsequent promises from being accelerated.
    seed_promise(io, kPromise1);
    controller.tick(1, kBase, true);
    CHECK(cadence(io, kPromise1) == 0.f, "reused promise class remains active");
    CHECK(controller.neutralized() == 2u, "reused promise write counted");

    // The PlayerLoop statics/runners may be rebuilt while the session stays
    // bound. The next tick must follow the new chain rather than old addresses.
    seed_chain(io, kStatics2, kRunners2, kRunner2, kItems2, kPromise2);
    seed_promise(io, kPromise2);
    controller.tick(1, kBase, true);
    CHECK(cadence(io, kPromise2) == 0.f, "rebuilt PlayerLoop chain recovered");
    CHECK(controller.neutralized() == 3u, "rebuilt chain write counted");

    // A structurally similar promise with another cadence is not touched.
    seed_promise(io, kPromise2, 0.20f);
    controller.tick(1, kBase, true);
    CHECK(cadence(io, kPromise2) == 0.20f, "other cadence rejected");

    // Four failed guarded writes clear cached addresses. Once the heap gate is
    // available again, the controller resolves the live chain and resumes on
    // its own without toggling the feature.
    seed_promise(io, kPromise2);
    io.allow_write = false;
    for (int i = 0; i < 4; ++i) controller.tick(1, kBase, true);
    CHECK(controller.status == viewmodel::Status::Failed, "write failures reported");
    io.allow_write = true;
    controller.tick(1, kBase, true);
    CHECK(controller.status == viewmodel::Status::Active, "write failure self-heals");
    CHECK(cadence(io, kPromise2) == 0.f, "recovered chain neutralized");

    controller.tick(1, kBase, false);
    CHECK(controller.status == viewmodel::Status::Off, "disable -> Off");

    // A transient read failure on Il2CppClass::name must not cache a real
    // promise as a permanent non-promise.
    FakeIO unreadable_promise_name;
    seed_scene(unreadable_promise_name);
    for (std::size_t i = 0; i < sizeof(uint64_t); ++i)
        unreadable_promise_name.mem.erase(kDelayClass + go_active::IL2CPP_CLASS_NAME + i);
    fast_loot::Controller<FakeIO> structural_controller(unreadable_promise_name);
    structural_controller.tick(1, kBase, true);
    CHECK(structural_controller.status == viewmodel::Status::Active,
          "unreadable promise class name uses cadence fallback");
    CHECK(cadence(unreadable_promise_name, kPromise1) == 0.f,
          "promise still neutralized after class-name read failure");

    // The helper TypeInfo RVA is authoritative when metadata strings cannot be
    // read; a failed name-pointer read is transient, not a build mismatch.
    FakeIO unreadable_helper_name;
    seed_scene(unreadable_helper_name);
    for (std::size_t i = 0; i < sizeof(uint64_t); ++i)
        unreadable_helper_name.mem.erase(kHelperClass + go_active::IL2CPP_CLASS_NAME + i);
    fast_loot::Controller<FakeIO> helper_controller(unreadable_helper_name);
    helper_controller.tick(1, kBase, true);
    CHECK(helper_controller.status == viewmodel::Status::Active,
          "unreadable helper class name trusts validated TypeInfo RVA");
    CHECK(cadence(unreadable_helper_name, kPromise1) == 0.f,
          "promise neutralized with unreadable helper name");

    if (failures) { std::printf("%d FAILURES\n", failures); return 1; }
    std::puts("ALL FAST-LOOT TESTS PASS");
    return 0;
}

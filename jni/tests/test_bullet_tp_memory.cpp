#include "bullet_tp_memory.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct FakeIO {
    uint64_t begin = 0x10000;
    uint64_t projectile = 0x10800;
    std::vector<uint8_t> memory = std::vector<uint8_t>(0x10000);
    unsigned writes = 0;
    unsigned exact_calls = 0;
    unsigned range_reads = 0;
    bool is_bound = true;

    template<class T> void put(uint64_t address, const T& value) {
        assert(address >= begin && address + sizeof(T) <= begin + memory.size());
        std::memcpy(memory.data() + static_cast<size_t>(address - begin), &value, sizeof(T));
    }
    template<class T> T get(uint64_t address) const {
        assert(address >= begin && address + sizeof(T) <= begin + memory.size());
        T value{};
        std::memcpy(&value, memory.data() + static_cast<size_t>(address - begin), sizeof(T));
        return value;
    }
    bool bound() const { return is_bound; }
    bool read_range(uint64_t address, void* out, size_t size) {
        ++range_reads;
        if (!out || address < begin || address + size > begin + memory.size()) return false;
        std::memcpy(out, memory.data() + static_cast<size_t>(address - begin), size);
        return true;
    }
    uint64_t object_class(uint64_t address) { return get<uint64_t>(address); }
    bool exact(uint64_t address, const char* name, const char* namespaze) {
        ++exact_calls;
        return address == projectile &&
            std::strcmp(name, bullet_tp::kClassName) == 0 &&
            std::strcmp(namespaze, bullet_tp::kNamespace) == 0;
    }
    template<class T> bool write_heap(uint64_t address, const T& value) {
        if (address < begin || address + sizeof(T) > begin + memory.size()) return false;
        put(address, value);
        ++writes;
        return true;
    }
};

namespace {
constexpr uint64_t kPid = 41;
constexpr uint64_t kModuleBase = 0x400000;
const std::string kMaps = "00010000-00020000 rw-p 00000000 00:00 0 [heap]\n";

void install_projectile(FakeIO& io, uint64_t address, uint64_t klass,
                        bullet_tp::Vec3 current, bullet_tp::Vec3 previous) {
    io.put<uint64_t>(address, klass);
    io.put<float>(address + game_offsets::BULLET_PROJECTILE_SLOWDOWN, 0.5f);
    io.put<float>(address + game_offsets::BULLET_PROJECTILE_MINSPEED, 100.f);
    io.put<float>(address + game_offsets::BULLET_PROJECTILE_TRACESIZE, 1.f);
    io.put(address + game_offsets::BULLET_PROJECTILE_POSITION, current);
    io.put(address + game_offsets::BULLET_PROJECTILE_PREVIOUS_POSITION, previous);
}

void test_offsets_and_samples() {
    assert(game_offsets::BULLET_PROJECTILE_SLOWDOWN == 0x20);
    assert(game_offsets::BULLET_PROJECTILE_MINSPEED == 0x24);
    assert(game_offsets::BULLET_PROJECTILE_TRACESIZE == 0x30);
    assert(game_offsets::BULLET_PROJECTILE_POSITION == 0x54);
    assert(game_offsets::BULLET_PROJECTILE_PREVIOUS_POSITION == 0x60);

    std::vector<uint8_t> bytes(bullet_tp::kObjectSpan);
    const float slowdown = 0.5f, speed = 150.f, trace = 0.75f;
    const bullet_tp::Vec3 current{12.f, 4.f, -20.f};
    std::memcpy(bytes.data() + game_offsets::BULLET_PROJECTILE_SLOWDOWN, &slowdown, sizeof(slowdown));
    std::memcpy(bytes.data() + game_offsets::BULLET_PROJECTILE_MINSPEED, &speed, sizeof(speed));
    std::memcpy(bytes.data() + game_offsets::BULLET_PROJECTILE_TRACESIZE, &trace, sizeof(trace));
    std::memcpy(bytes.data() + game_offsets::BULLET_PROJECTILE_POSITION, &current, sizeof(current));
    bullet_tp::Vec3 sample{};
    assert(bullet_tp::sample_candidate(bytes.data(), bytes.size(), sample));
    assert(bullet_tp::same_vec(sample, current));
    assert(!bullet_tp::candidate_fields_valid(0.5f, 150.f, 0.f, current));
    assert(!bullet_tp::world_vec({9000.f, 0.f, 0.f}));
    assert(bullet_tp::world_vec({0.f, 0.f, 0.f}));
}

void test_heap_range_filter() {
    const std::string maps =
        "100000-110000 rw-p 00000000 00:00 0 [heap]\n"
        "200000-201000 rw-p 00000000 00:00 0 /system/lib64/libgame.so\n"
        "201000-202000 rw-p 00000000 00:00 0\n"
        "300000-301000 rw-p 00000000 00:00 0 [anon:UnityWhatever]\n"
        "400000-401000 rw-p 00000000 00:00 0 [anon:scudo:primary]\n"
        "500000-501000 rw-s 00000000 00:00 0 [heap]\n";
    std::vector<bullet_tp::Range> ranges;
    bullet_tp::MapDiagnostics diagnostics{};
    assert(bullet_tp::collect_heap_ranges(maps, ranges, &diagnostics));
    assert(ranges.size() == 2);
    assert(diagnostics.accepted == 2 && diagnostics.accepted_bytes == 0x11000);
    assert(diagnostics.file_backed == 1 && diagnostics.file_tail == 1);
    assert(diagnostics.named_regions == 1 && diagnostics.bad_permissions == 1);
    assert(std::strcmp(diagnostics.rejected_names[0], "[anon:UnityWhatever]") == 0);
    assert(ranges[0].begin == 0x100000 && ranges[0].end == 0x110000);
    assert(ranges[1].begin == 0x400000 && ranges[1].end == 0x401000);
    assert(!bullet_tp::collect_heap_ranges("bad maps header\n", ranges));
    assert(ranges.empty());
}

void test_controller_uses_io_and_tracks_candidates() {
    FakeIO io;
    const uint64_t bullet_class = 0x70000;
    install_projectile(io, io.projectile, bullet_class, {1.f, 2.f, 3.f}, {0.5f, 2.f, 3.f});
    install_projectile(io, 0x11800, bullet_class + 0x1000, {4.f, 5.f, 6.f}, {4.f, 5.f, 6.f});
    bullet_tp::Controller<FakeIO> controller(io);
    const bullet_tp::Vec3 first_target{10.f, 3.f, 20.f};
    assert(controller.tick(true, static_cast<int>(kPid), kModuleBase, 0x90000,
                           first_target, &kMaps, 1.0) == 1);
    assert(io.exact_calls == 1);
    assert(io.writes == 2);
    assert(bullet_tp::same_vec(io.get<bullet_tp::Vec3>(io.projectile + game_offsets::BULLET_PROJECTILE_POSITION), first_target));
    assert(bullet_tp::same_vec(io.get<bullet_tp::Vec3>(io.projectile + game_offsets::BULLET_PROJECTILE_PREVIOUS_POSITION), first_target));

    const bullet_tp::Vec3 second_target{30.f, 8.f, 40.f};
    assert(controller.tick(true, static_cast<int>(kPid), kModuleBase, 0x91000,
                           second_target, &kMaps, 1.016) == 1);
    assert(io.exact_calls == 1);
    assert(io.writes == 4);
    assert(bullet_tp::same_vec(io.get<bullet_tp::Vec3>(io.projectile + game_offsets::BULLET_PROJECTILE_POSITION), second_target));
    const auto diagnostics = controller.diagnostics();
    assert(diagnostics.scan_cycles == 1 && diagnostics.scan_chunks == 1);
    assert(diagnostics.candidates_with_valid_fields >= 2);
    assert(diagnostics.exact_checks == 1 && diagnostics.exact_matches == 1);
    assert(diagnostics.projectiles_added == 1 && diagnostics.tracked_projectiles == 1);
    assert(diagnostics.write_pairs_ok == 2 && diagnostics.current_writes_ok == 2);

    assert(controller.tick(false, static_cast<int>(kPid), kModuleBase, 0,
                           {}, &kMaps, 1.032) == 0);
    assert(io.writes == 4);
}

void test_scan_is_incremental() {
    FakeIO io;
    io.memory.resize(0x500000);
    const std::string maps = "00010000-00510000 rw-p 00000000 00:00 0 [heap]\n";
    install_projectile(io, io.projectile, 0x70000, {1.f, 2.f, 3.f}, {1.f, 2.f, 3.f});
    bullet_tp::Controller<FakeIO> controller(io);

    assert(controller.tick(true, static_cast<int>(kPid), kModuleBase, 0x90000,
                           {10.f, 3.f, 20.f}, &maps, 2.0) == 1);
    assert(io.range_reads == 7); // six overlapping scan chunks and one candidate revalidation
    assert(io.exact_calls == 1 && io.writes == 2);

    controller.tick(true, static_cast<int>(kPid), kModuleBase, 0x90000,
                    {11.f, 4.f, 21.f}, &maps, 2.016);
    assert(io.range_reads == 8); // no new scan step before the 50 ms interval
    assert(io.writes == 4);
    const auto diagnostics = controller.diagnostics();
    assert(diagnostics.scan_chunks == 6 && !diagnostics.scan_active);
    assert(diagnostics.scan_bytes >= 0x500000);
}

void test_scan_budget_caps_small_ranges() {
    FakeIO io;
    std::string maps;
    for (uint64_t index = 0; index < 20; ++index) {
        const uint64_t begin = 0x300000 + index * 0x2000;
        char line[128]{};
        std::snprintf(line, sizeof(line), "%llx-%llx rw-p 00000000 00:00 0 [anon:scudo:primary]\n",
            static_cast<unsigned long long>(begin),
            static_cast<unsigned long long>(begin + 0x1000));
        maps += line;
    }
    bullet_tp::Controller<FakeIO> controller(io);
    controller.tick(true, static_cast<int>(kPid), kModuleBase, 0x90000,
                    {10.f, 3.f, 20.f}, &maps, 3.0);
    const auto diagnostics = controller.diagnostics();
    assert(io.range_reads == bullet_tp::kScanChunksPerStep);
    assert(diagnostics.scan_chunks == bullet_tp::kScanChunksPerStep);
    assert(diagnostics.scan_read_failures == bullet_tp::kScanChunksPerStep);
    assert(diagnostics.scan_active && diagnostics.maps.accepted == 20);
}

void test_config_v7_compatibility() {
    ViewmodelSettings settings;
    settings.bullet_tp = true;
    settings.no_ballistics = true;
    const auto encoded = viewmodel::encode(settings);
    assert(encoded.version == 7);
    assert((encoded.flags & 4096u) != 0);
    assert((encoded.flags & 2048u) != 0);
    auto decoded = viewmodel::decode(&encoded, sizeof(encoded));
    assert(decoded.bullet_tp && decoded.no_ballistics);

    auto v6 = encoded;
    v6.version = 6;
    v6.flags &= ~4096u;
    decoded = viewmodel::decode(&v6, sizeof(v6));
    assert(!decoded.bullet_tp && decoded.no_ballistics);

    v6.flags |= 4096u;
    decoded = viewmodel::decode(&v6, sizeof(v6));
    assert(!decoded.bullet_tp && !decoded.no_ballistics);
}
}

int main() {
    test_offsets_and_samples();
    test_heap_range_filter();
    test_controller_uses_io_and_tracks_candidates();
    test_scan_is_incremental();
    test_scan_budget_caps_small_ranges();
    test_config_v7_compatibility();
    std::puts("ALL BULLET-TP TESTS PASS");
}

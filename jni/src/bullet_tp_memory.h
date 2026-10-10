#pragma once
#include "game_offsets.h"
#include "viewmodel.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace bullet_tp {
struct Vec3 { float x = 0.f, y = 0.f, z = 0.f; };
static_assert(sizeof(Vec3) == 12, "Unity Vector3 layout");

struct Range { uint64_t begin = 0, end = 0; };
struct Projectile { uint64_t address = 0, klass = 0; };
struct MapDiagnostics {
    size_t lines = 0, parse_failures = 0, rw_private = 0;
    size_t file_backed = 0, bad_permissions = 0, file_tail = 0;
    size_t named_regions = 0, bad_size = 0, accepted = 0;
    uint64_t accepted_bytes = 0;
    char rejected_names[4][64]{};
    size_t rejected_name_counts[4]{};
};

struct Diagnostics {
    MapDiagnostics maps{};
    uint64_t scan_cycles = 0, scan_chunks = 0, scan_read_failures = 0;
    uint64_t scan_bytes = 0, candidates_with_valid_fields = 0;
    uint64_t valid_class_pointers = 0, invalid_class_pointers = 0;
    uint64_t exact_checks = 0, exact_matches = 0, exact_rejects = 0;
    uint64_t projectiles_added = 0, apply_candidates = 0;
    uint64_t apply_class_mismatches = 0, apply_read_failures = 0;
    uint64_t apply_invalid_fields = 0, already_at_target = 0;
    uint64_t current_write_attempts = 0, previous_write_attempts = 0;
    uint64_t current_writes_ok = 0, previous_writes_ok = 0, write_pairs_ok = 0;
    uint64_t current_write_failures = 0, previous_write_failures = 0;
    uint64_t last_candidate = 0, last_class = 0, projectile_class = 0;
    size_t heap_ranges = 0, tracked_projectiles = 0;
    bool scan_active = false, class_known = false;
};

inline constexpr const char* kClassName = "BulletProjectile";
inline constexpr const char* kNamespace = "HyperHug.Games.Oxide.Features.Weapons";
inline constexpr size_t kObjectSpan = game_offsets::BULLET_PROJECTILE_PREVIOUS_POSITION + sizeof(Vec3);
inline constexpr size_t kChunkSize = 1u << 20;
inline constexpr size_t kChunkOverlap = (kObjectSpan + 7u) & ~size_t(7u);
inline constexpr double kScanStepIntervalSeconds = 0.05;
inline constexpr double kFullScanIntervalSeconds = 0.25;
inline constexpr size_t kScanChunksPerStep = 8;

inline bool finite_vec(Vec3 p) {
    return viewmodel::finite(p.x) && viewmodel::finite(p.y) && viewmodel::finite(p.z);
}

inline bool world_vec(Vec3 p) {
    return finite_vec(p) && p.x >= -8192.f && p.x <= 8192.f &&
        p.y >= -500.f && p.y <= 1000.f && p.z >= -8192.f && p.z <= 8192.f;
}

inline bool candidate_fields_valid(float slowdown, float min_speed, float trace_size, Vec3 position) {
    return viewmodel::finite(slowdown) && slowdown >= 0.f && slowdown <= 5.f &&
        viewmodel::finite(min_speed) && min_speed >= 0.f && min_speed <= 2000.f &&
        viewmodel::finite(trace_size) && trace_size > 0.f && trace_size <= 2.f &&
        world_vec(position);
}

inline bool sample_candidate(const uint8_t* bytes, size_t size, Vec3& current, Vec3* previous = nullptr) {
    using namespace game_offsets;
    if (!bytes || size < kObjectSpan) return false;
    float slowdown = 0.f, min_speed = 0.f, trace_size = 0.f;
    std::memcpy(&slowdown, bytes + BULLET_PROJECTILE_SLOWDOWN, sizeof(slowdown));
    std::memcpy(&min_speed, bytes + BULLET_PROJECTILE_MINSPEED, sizeof(min_speed));
    std::memcpy(&trace_size, bytes + BULLET_PROJECTILE_TRACESIZE, sizeof(trace_size));
    std::memcpy(&current, bytes + BULLET_PROJECTILE_POSITION, sizeof(current));
    if (previous)
        std::memcpy(previous, bytes + BULLET_PROJECTILE_PREVIOUS_POSITION, sizeof(*previous));
    return candidate_fields_valid(slowdown, min_speed, trace_size, current);
}

inline bool same_vec(Vec3 a, Vec3 b) {
    return std::memcmp(&a, &b, sizeof(Vec3)) == 0;
}

inline bool heap_name_allowed(const char* name, size_t length) {
    auto equals = [&](const char* expected) {
        const size_t expected_length = std::strlen(expected);
        return length == expected_length && std::memcmp(name, expected, length) == 0;
    };
    return length == 0 || equals("[heap]") || equals("[anon:libc_malloc]") ||
        equals("[anon:scudo:primary]") || equals("[anon:scudo:secondary]");
}

inline bool collect_heap_ranges(const std::string& maps, std::vector<Range>& ranges,
                                MapDiagnostics* diagnostics = nullptr) {
    ranges.clear();
    if (diagnostics) *diagnostics = {};
    uint64_t previous_end = 0;
    bool file_tail = false;
    size_t position = 0;
    constexpr uint64_t kFileTailArenaMin = 8ull * 1024 * 1024;
    constexpr uint64_t kMaxRangeSize = 64ull * 1024 * 1024;

    auto note_rejected_name = [&](const char* name, size_t length) {
        if (!diagnostics || !length) return;
        const size_t copied = std::min(length, sizeof(diagnostics->rejected_names[0]) - 1);
        for (size_t i = 0; i < 4; ++i) {
            if (diagnostics->rejected_names[i][0] &&
                std::strncmp(diagnostics->rejected_names[i], name, copied) == 0 &&
                diagnostics->rejected_names[i][copied] == '\0') {
                ++diagnostics->rejected_name_counts[i];
                return;
            }
        }
        for (size_t i = 0; i < 4; ++i) {
            if (diagnostics->rejected_names[i][0]) continue;
            std::memcpy(diagnostics->rejected_names[i], name, copied);
            diagnostics->rejected_names[i][copied] = '\0';
            diagnostics->rejected_name_counts[i] = 1;
            return;
        }
    };

    while (position < maps.size()) {
        size_t line_end = maps.find('\n', position);
        if (line_end == std::string::npos) line_end = maps.size();
        const char* line = maps.data() + position;
        const size_t line_length = line_end - position;
        position = line_end + 1;
        if (!line_length) continue;
        if (diagnostics) ++diagnostics->lines;

        char header[160];
        const size_t header_length = std::min(line_length, sizeof(header) - 1);
        std::memcpy(header, line, header_length);
        header[header_length] = '\0';

        unsigned long long begin = 0, end = 0, offset = 0, inode = 0;
        unsigned major = 0, minor = 0;
        char permissions[5]{};
        int consumed = 0;
        if (std::sscanf(header, "%llx-%llx %4s %llx %x:%x %llu %n",
                        &begin, &end, permissions, &offset, &major, &minor, &inode, &consumed) != 7 ||
            !consumed || static_cast<size_t>(consumed) > line_length ||
            end <= begin || begin < previous_end) {
            ranges.clear();
            if (diagnostics) {
                ++diagnostics->parse_failures;
                diagnostics->accepted = 0;
                diagnostics->accepted_bytes = 0;
            }
            return false;
        }

        const char* name = line + consumed;
        size_t name_length = line_length - static_cast<size_t>(consumed);
        while (name_length && (name[name_length - 1] == ' ' || name[name_length - 1] == '\r'))
            --name_length;
        const bool file = inode != 0 || std::memchr(name, '/', name_length) != nullptr;
        const bool anonymous = heap_name_allowed(name, name_length);
        const bool adjacent_to_file = file || (begin == previous_end && file_tail);
        file_tail = adjacent_to_file;
        previous_end = static_cast<uint64_t>(end);

        const uint64_t size = static_cast<uint64_t>(end - begin);
        const bool rw_private = std::strcmp(permissions, "rw-p") == 0;
        if (diagnostics && rw_private) ++diagnostics->rw_private;
        if (begin >= 0x10000 && rw_private && !offset && !major && !minor && !inode && !file &&
            anonymous && !(adjacent_to_file && size < kFileTailArenaMin) &&
            size >= 0x1000 && size <= kMaxRangeSize) {
            ranges.push_back({static_cast<uint64_t>(begin), static_cast<uint64_t>(end)});
            if (diagnostics) {
                ++diagnostics->accepted;
                diagnostics->accepted_bytes += size;
            }
            continue;
        }
        if (!diagnostics) continue;
        if (inode || offset || major || minor || file) ++diagnostics->file_backed;
        else if (!rw_private) ++diagnostics->bad_permissions;
        else if (adjacent_to_file && size < kFileTailArenaMin) ++diagnostics->file_tail;
        else if (!anonymous) {
            ++diagnostics->named_regions;
            note_rejected_name(name, name_length);
        } else if (size < 0x1000 || size > kMaxRangeSize || begin < 0x10000) {
            ++diagnostics->bad_size;
        }
    }
    return true;
}

template<class IO> class Controller {
    IO& io_;
    std::vector<Projectile> projectiles_;
    std::vector<uint8_t> chunk_ = std::vector<uint8_t>(kChunkSize);
    std::vector<Range> scan_ranges_;
    std::unordered_set<uint64_t> seen_;
    uint64_t module_base_ = 0;
    uint64_t projectile_class_ = 0;
    size_t scan_range_index_ = 0;
    uint64_t scan_offset_ = 0;
    int pid_ = -1;
    bool scan_active_ = false;
    double last_scan_ = -1e9;
    double last_scan_step_ = -1e9;
    Diagnostics diagnostics_{};

    void begin_scan(const std::string& maps, double now) {
        last_scan_ = now;
        scan_active_ = false;
        scan_ranges_.clear();
        scan_range_index_ = 0;
        scan_offset_ = 0;
        ++diagnostics_.scan_cycles;
        if (!collect_heap_ranges(maps, scan_ranges_, &diagnostics_.maps)) return;
        diagnostics_.heap_ranges = scan_ranges_.size();
        seen_.clear();
        seen_.reserve(projectiles_.size() + 16);
        for (const Projectile& projectile : projectiles_) seen_.insert(projectile.address);
        scan_active_ = !scan_ranges_.empty();
    }

    void scan_chunk(const Range& range, uint64_t offset, size_t amount) {
        if (amount < kObjectSpan) return;
        ++diagnostics_.scan_chunks;
        diagnostics_.scan_bytes += amount;
        if (!io_.read_range(range.begin + offset, chunk_.data(), amount)) {
            ++diagnostics_.scan_read_failures;
            return;
        }
        for (size_t index = 0; index + kObjectSpan <= amount; index += sizeof(uint64_t)) {
            Vec3 position{};
            if (!sample_candidate(chunk_.data() + index, amount - index, position)) continue;
            ++diagnostics_.candidates_with_valid_fields;
            const uint64_t address = range.begin + offset + index;
            if (seen_.find(address) != seen_.end()) continue;

            const uint64_t klass = io_.object_class(address);
            if (klass < 0x10000) {
                ++diagnostics_.invalid_class_pointers;
                continue;
            }
            ++diagnostics_.valid_class_pointers;
            diagnostics_.last_candidate = address;
            diagnostics_.last_class = klass;
            if (!projectile_class_) {
                ++diagnostics_.exact_checks;
                if (!io_.exact(address, kClassName, kNamespace)) {
                    ++diagnostics_.exact_rejects;
                    continue;
                }
                ++diagnostics_.exact_matches;
                projectile_class_ = klass;
            }
            if (klass != projectile_class_) continue;
            seen_.insert(address);
            projectiles_.push_back({address, klass});
            ++diagnostics_.projectiles_added;
        }
    }

    void scan_step(double now) {
        // A bounded worker budget finds short-lived bullets without blocking the ESP frame.
        size_t chunks_remaining = kScanChunksPerStep;
        while (scan_active_ && chunks_remaining && scan_range_index_ < scan_ranges_.size()) {
            const Range& range = scan_ranges_[scan_range_index_];
            const uint64_t range_size = range.end - range.begin;
            const uint64_t remaining = range_size - scan_offset_;
            const size_t amount = static_cast<size_t>(std::min<uint64_t>(kChunkSize, remaining));
            scan_chunk(range, scan_offset_, amount);
            --chunks_remaining;
            if (remaining <= kChunkSize) {
                ++scan_range_index_;
                scan_offset_ = 0;
            } else {
                scan_offset_ += kChunkSize - kChunkOverlap;
            }
        }
        if (scan_active_ && scan_range_index_ >= scan_ranges_.size()) {
            scan_active_ = false;
            scan_ranges_.clear();
            scan_range_index_ = 0;
            scan_offset_ = 0;
            last_scan_ = now;
        }
    }

    int apply(Vec3 target) {
        int written = 0;
        for (auto it = projectiles_.begin(); it != projectiles_.end();) {
            ++diagnostics_.apply_candidates;
            if (it->klass != projectile_class_ || io_.object_class(it->address) != it->klass) {
                ++diagnostics_.apply_class_mismatches;
                seen_.erase(it->address);
                it = projectiles_.erase(it);
                continue;
            }
            std::array<uint8_t, kObjectSpan> bytes{};
            if (!io_.read_range(it->address, bytes.data(), bytes.size())) {
                ++diagnostics_.apply_read_failures;
                ++it;
                continue;
            }
            Vec3 current{}, previous{};
            if (!sample_candidate(bytes.data(), bytes.size(), current, &previous)) {
                ++diagnostics_.apply_invalid_fields;
                seen_.erase(it->address);
                it = projectiles_.erase(it);
                continue;
            }
            if (same_vec(current, target) && same_vec(previous, target)) {
                ++diagnostics_.already_at_target;
                ++it;
                continue;
            }
            ++diagnostics_.current_write_attempts;
            const bool current_written = io_.write_heap(
                it->address + game_offsets::BULLET_PROJECTILE_POSITION, target);
            if (current_written) ++diagnostics_.current_writes_ok;
            else ++diagnostics_.current_write_failures;
            ++diagnostics_.previous_write_attempts;
            const bool previous_written = io_.write_heap(
                it->address + game_offsets::BULLET_PROJECTILE_PREVIOUS_POSITION, target);
            if (previous_written) ++diagnostics_.previous_writes_ok;
            else ++diagnostics_.previous_write_failures;
            if (current_written && previous_written) {
                ++written;
                ++diagnostics_.write_pairs_ok;
            }
            ++it;
        }
        return written;
    }

public:
    explicit Controller(IO& io) : io_(io) {}

    void reset() {
        projectiles_.clear();
        scan_ranges_.clear();
        seen_.clear();
        module_base_ = 0;
        projectile_class_ = 0;
        scan_range_index_ = 0;
        scan_offset_ = 0;
        pid_ = -1;
        scan_active_ = false;
        last_scan_ = -1e9;
        last_scan_step_ = -1e9;
        diagnostics_ = {};
    }

    Diagnostics diagnostics() const {
        Diagnostics result = diagnostics_;
        result.heap_ranges = scan_ranges_.size();
        result.tracked_projectiles = projectiles_.size();
        result.projectile_class = projectile_class_;
        result.scan_active = scan_active_;
        result.class_known = projectile_class_ != 0;
        return result;
    }

    int tick(bool enabled, int pid, uint64_t module_base, uint64_t target_id,
             Vec3 target, const std::string* maps, double now) {
        if (!enabled) {
            reset();
            return 0;
        }
        if (!io_.bound() || pid <= 0 || !module_base || !target_id || !world_vec(target) || !maps)
            return 0;
        if (pid_ != pid || module_base_ != module_base) {
            reset();
            pid_ = pid;
            module_base_ = module_base;
        }
        if (now < last_scan_ || now < last_scan_step_) {
            scan_active_ = false;
            scan_ranges_.clear();
            scan_range_index_ = 0;
            scan_offset_ = 0;
            last_scan_ = -1e9;
            last_scan_step_ = -1e9;
        }
        if (!scan_active_ && now - last_scan_ >= kFullScanIntervalSeconds)
            begin_scan(*maps, now);
        if (scan_active_ && now - last_scan_step_ >= kScanStepIntervalSeconds) {
            last_scan_step_ = now;
            scan_step(now);
        }
        return apply(target);
    }
};
} // namespace bullet_tp

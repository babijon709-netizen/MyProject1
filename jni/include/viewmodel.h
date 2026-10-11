#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

// Vertical world FOV; hands are one static position shared by all poses, cm.
struct ViewmodelSettings {
    bool fov_enabled = false;
    bool hands_enabled = false;
    float fov = 90.f;
    float x = 0.f, y = 0.f, z = 0.f;
    int fps_limit = 0; // 0 = original game limit; otherwise 90 / 120 / 144
    // «Без отдачи»: гасит амплитуду в RecoilConfig хитскан-ствола в руках
    // (реализация: no_recoil_memory.h, тик из esp_viewmodel_tick).
    bool no_recoil_button = false;
    bool no_spread = false;
    bool no_ballistics = false;
    bool sprint_while_aiming = false;
    bool sprint_while_reloading = false;
    bool bow_no_delay = false;
    bool projectile_speed_enabled = false;
    bool bow_draw_speed_enabled = false;
    bool bow_draw_power_enabled = false;
    float projectile_speed_multiplier = 1.f;
    float bow_draw_speed_multiplier = 1.f;
    float bow_draw_power_multiplier = 1.f;
    // Быстрый подбор («Забрать все» без пауз): не трогает код игры — обходит
    // раннеры плеер-лупа UniTask и обнуляет точный 0.35f размер активных
    // DelayPromise, на которых игра ждёт между предметами при сборе лута.
    // Реализация: fast_loot_memory.h, тик из esp_viewmodel_tick.
    bool fast_loot = false;
};

namespace viewmodel {
// The Android build uses -ffast-math; do not rely on isfinite under that flag.
inline bool finite(float v) {
    uint32_t bits; std::memcpy(&bits, &v, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}
inline float bounded(float v, float lo, float hi, float fallback) {
    return !finite(v) ? fallback : (v < lo ? lo : (v > hi ? hi : v));
}
inline int sanitize_fps_limit(int fps) {
    return fps == 90 || fps == 120 || fps == 144 ? fps : 0;
}
inline ViewmodelSettings sanitize(ViewmodelSettings s) {
    s.fps_limit = sanitize_fps_limit(s.fps_limit);
    s.fov = bounded(s.fov, 50.f, 120.f, 90.f);
    s.x = bounded(s.x, -50.f, 50.f, 0.f);
    s.y = bounded(s.y, -50.f, 50.f, 0.f);
    s.z = bounded(s.z, -50.f, 50.f, 0.f);
    s.projectile_speed_multiplier = bounded(s.projectile_speed_multiplier, 0.1f, 10.f, 1.f);
    s.bow_draw_speed_multiplier = bounded(s.bow_draw_speed_multiplier, 0.1f, 10.f, 1.f);
    s.bow_draw_power_multiplier = bounded(s.bow_draw_power_multiplier, 0.1f, 10.f, 1.f);
    return s;
}
// Optional settings after the stable ConfigExtension. Missing, truncated, or
// unknown weapon trailers load disabled defaults, never stale state.
struct ConfigExtension {
    uint32_t magic = 0x31574d56u; // VMW1
    // v4 added aim lead, v5 kept the 36-byte layout, and v6 assigns bit 11
    // to no-ballistics. Bit 10 remains retired. v1–v3 were 32 bytes.
    uint32_t version = 6;
    uint32_t size = 36;
    uint32_t flags = 0;
    float fov = 90.f, x = 0.f, y = 0.f, z = 0.f;
    float aim_lead_ms = 0.f;   // v4: aim prediction lead, 0..150 ms (0 = off)
};
static_assert(sizeof(ConfigExtension) == 36, "stable config extension");

// Appended after ConfigExtension. Its presence is optional so v1-v6 profiles
// keep their original offsets and still decode with disabled weapon controls.
struct WeaponConfigExtension {
    uint32_t magic = 0x31545756u; // VWT1
    uint32_t version = 1;
    uint32_t size = 32;
    uint32_t flags = 0;
    float projectile_speed_multiplier = 1.f;
    float bow_draw_speed_multiplier = 1.f;
    float bow_draw_power_multiplier = 1.f;
    uint32_t reserved = 0;
};
static_assert(sizeof(WeaponConfigExtension) == 32, "stable weapon trailer");

inline WeaponConfigExtension encode_weapon(ViewmodelSettings s) {
    s = sanitize(s);
    WeaponConfigExtension e;
    e.flags = (s.sprint_while_aiming ? 1u : 0u) |
              (s.sprint_while_reloading ? 2u : 0u) |
              (s.bow_no_delay ? 4u : 0u) |
              (s.projectile_speed_enabled ? 8u : 0u) |
              (s.bow_draw_speed_enabled ? 16u : 0u) |
              (s.bow_draw_power_enabled ? 32u : 0u);
    e.projectile_speed_multiplier = s.projectile_speed_multiplier;
    e.bow_draw_speed_multiplier = s.bow_draw_speed_multiplier;
    e.bow_draw_power_multiplier = s.bow_draw_power_multiplier;
    return e;
}

inline bool decode_weapon(const void* bytes, size_t size, ViewmodelSettings& s) {
    if (!bytes || size != sizeof(WeaponConfigExtension)) return false;
    WeaponConfigExtension e;
    std::memcpy(&e, bytes, sizeof(e));
    if (e.magic != 0x31545756u || e.version != 1 || e.size != sizeof(e) ||
        (e.flags & ~63u) != 0 || e.reserved != 0) return false;
    s.sprint_while_aiming = (e.flags & 1u) != 0;
    s.sprint_while_reloading = (e.flags & 2u) != 0;
    s.bow_no_delay = (e.flags & 4u) != 0;
    s.projectile_speed_enabled = (e.flags & 8u) != 0;
    s.bow_draw_speed_enabled = (e.flags & 16u) != 0;
    s.bow_draw_power_enabled = (e.flags & 32u) != 0;
    s.projectile_speed_multiplier = e.projectile_speed_multiplier;
    s.bow_draw_speed_multiplier = e.bow_draw_speed_multiplier;
    s.bow_draw_power_multiplier = e.bow_draw_power_multiplier;
    s = sanitize(s);
    return true;
}

inline ConfigExtension encode(ViewmodelSettings s) {
    s = sanitize(s);
    ConfigExtension e;
    // Bits 2..3: 0 = default, 1 = 144 (legacy toggle), 2 = 90, 3 = 120.
    const uint32_t fps_mode = s.fps_limit == 144 ? 1u : s.fps_limit == 90 ? 2u : s.fps_limit == 120 ? 3u : 0u;
    e.flags = (s.fov_enabled ? 1u : 0u) | (s.hands_enabled ? 2u : 0u) | (fps_mode << 2) |
              (s.fast_loot ? 16u : 0u) | (s.no_recoil_button ? 32u : 0u) |
              (s.no_spread ? 64u : 0u) | (s.no_ballistics ? 2048u : 0u);
    e.fov = s.fov; e.x = s.x; e.y = s.y; e.z = s.z;
    return e;
}
inline ViewmodelSettings decode(const void* bytes, size_t size) {
    ViewmodelSettings s;
    // 32 = v1–v3 layout (no aim lead); 36 = v4+. A short record memcpy's into
    // the 36-byte struct and leaves aim_lead_ms at its default 0.
    if (size != 32 && size != sizeof(ConfigExtension)) return s;
    ConfigExtension e; std::memcpy(&e, bytes, size);
    // v1: discard retired flags 4..6. v2 keeps bit 5 as the UI-only recoil
    // toggle and bit 6 for no-spread. v3 assigns bit 4 to fast pickup; an
    // older v1/v2 profile carrying retired bit 4 must not enable it.
    const uint32_t allowed_flags = e.version >= 6 ? 4095u : 2047u;
    if (e.magic != 0x31574d56u || (e.version < 1 || e.version > 6) ||
        e.size != size || (e.flags & ~allowed_flags)) return s;
    s.fov_enabled = (e.flags & 1u) != 0;
    s.hands_enabled = (e.flags & 2u) != 0;
    const int fps_modes[] = {0, 144, 90, 120};
    s.fps_limit = fps_modes[(e.flags >> 2) & 3u];
    s.no_recoil_button = e.version >= 2 && (e.flags & 32u) != 0;
    s.no_spread = e.version >= 2 && (e.flags & 64u) != 0;
    s.no_ballistics = e.version >= 6 && (e.flags & 2048u) != 0;
    s.fast_loot = e.version >= 3 && (e.flags & 16u) != 0;
    s.fov = e.fov; s.x = e.x; s.y = e.y; s.z = e.z;
    return sanitize(s);
}
// Aim prediction lead from a v4 trailer, in milliseconds. Old (v1–v3) or
// malformed trailers read as 0 — prediction off, never a stale value.
inline float decode_aim_lead_ms(const void* bytes, size_t size) {
    if (size != sizeof(ConfigExtension)) return 0.f;
    ConfigExtension e; std::memcpy(&e, bytes, sizeof(e));
    if (e.magic != 0x31574d56u || (e.version < 4 || e.version > 6) || e.size != sizeof(e)) return 0.f;
    return (e.aim_lead_ms >= 0.f && e.aim_lead_ms <= 150.f) ? e.aim_lead_ms : 0.f;
}
enum class Status { Off, Waiting, Active, ADS, Failed };
struct Diagnostics {
    Status fov = Status::Off, hands = Status::Off;
    Status no_spread = Status::Off;
    Status fast_fire = Status::Off;
    Status movement_tuning = Status::Off;
    Status weapon_controls = Status::Off;
};
// Снимок состояния limiting FPS для меню: снимается контроллером при последнем
// тике (до 100 мс назад), дополнительных обращений к памяти игры не делает.
struct FpsUiInfo {
    int requested = 0;               // последний обработанный режим (0 = обычный)
    Status status = Status::Off;
    int field_value = 0;             // фактическое значение backing-поля targetFrameRate
    int pace_kind = 0;               // тип Android frame pacing (0 = заглушен)
    uint64_t build_id = 0;           // build ID найденной libunity (0 = mapping'а нет/не прочитан)
};
}

// Main-thread tick, independent of ESP toggles. No background writer.
void esp_viewmodel_tick(const ViewmodelSettings& settings);
viewmodel::Diagnostics esp_viewmodel_diagnostics();
// Снимок состояния разблокировки FPS для меню (выбран режим / применён / поле).
viewmodel::FpsUiInfo esp_viewmodel_fps_ui();
// restore=true only for a live connection (exit/build switch).
// restore=false on lost process/session: forget old addresses without writes.
void esp_viewmodel_reset(bool restore);

#pragma once
#include <cstdint>

namespace game_offsets {

// ─── Verified against the 2026-08 game build ────────────────────────────────
// dump.cs / script.json: custom il2cpp_dump.py, metadata v39 (XOR 0xa5c3f19d
// + triple permutation). libunity.so is Unity 6000.3.18f1 (see .note.unity).
//
// Native Unity Camera (via Camera::m_CachedPtr) — from libunity.so
// get_worldToCameraMatrix_Injected @ 0x5ac514 → helper 0xe1fe08 returns cam+0x70
// get_projectionMatrix_Injected    @ 0x5ac53c → helper 0xe1fe6c returns cam+0xB0
// (icall fns / helpers re-verified at the same addresses in the new libunity.)
// IMPORTANT: +0x70 / +0xB0 are LAZY caches (dirty flags @ +0x502 / +0x500).
// External process_vm_readv does NOT run the getters, so +0x70 goes stale when the
// camera moves — that is the ESP "drifts with camera" bug. Prefer rebuilding view
// from the live Transform at cam+0x20 (same pointer the dirty path uses).
inline constexpr std::uint64_t CAMERA_PROJECTION_MATRIX = 0xB0;  // Matrix4x4 projection (cached)
inline constexpr std::uint64_t CAMERA_VIEW_MATRIX       = 0x70;  // Matrix4x4 worldToCamera (cached, stale!)
inline constexpr std::uint64_t CAMERA_WORLD_TO_CLIP     = 0xF0;  // projection * worldToCamera product
inline constexpr std::uint64_t CAMERA_PREV_VIEW_PROJ    = 0x5C8; // previousViewProjection (frame-written)
inline constexpr std::uint64_t CAMERA_NATIVE_TRANSFORM  = 0x20;  // Transform* used by w2c dirty rebuild
inline constexpr std::uint64_t CAMERA_FOV_DEGREES       = 0x170; // get_fieldOfView storage
inline constexpr std::uint64_t CAMERA_ASPECT            = 0x4E0;
inline constexpr std::uint64_t CAMERA_NEAR_CLIP         = 0x454;
inline constexpr std::uint64_t CAMERA_FAR_CLIP          = 0x458;
inline constexpr std::uint64_t CAMERA_VIEW_DIRTY        = 0x502; // byte, set when w2c cache invalid
inline constexpr std::uint64_t CAMERA_PROJ_DIRTY        = 0x500; // byte, set when proj cache invalid
// Dirty-flag / FOV / aspect / clip offsets all re-verified by disassembling the new
// libunity.so: w2c helper 0xe1fe08 (ldrb [cam+0x502], rebuild via [cam+0x20] transform,
// matrix cam+0x70); proj helper 0xe1fe6c (ldrb [cam+0x500], fov 0x170, aspect 0x4E0,
// near 0x454, far 0x458); native worldToClip helper @ 0xe2b90c.

inline constexpr std::uint64_t MANAGED_CACHED_PTR = 0x10;

inline constexpr float PLAYER_HEIGHT          = 1.8F;
inline constexpr float PLAYER_BOX_WIDTH_RATIO = 0.40F;
inline constexpr float MIN_PLAYER_DISTANCE    = 0.0F;
inline constexpr float MAX_PLAYER_DISTANCE    = 300.0F;

// Il2CppClass* TypeInfo globals in libil2cpp.so .got (new dump, metadata v39).
// Found via codegen slots: slot load → ldr [klass+0xB8] (static_fields) → static
// offsets that match the dump.cs static layout for each class.
// Oxide.PlayerManager: slot read then [sf+0x00/0x08/0x10] (sleeping/active/client lists)
inline constexpr std::uint64_t PLAYER_MANAGER_TYPEINFO_RVA       = 0xD181268;
inline constexpr std::uint64_t PLAYER_MANAGER_STATIC_FIELDS_LIST = 0x10; // clientPlayerList

// Oxide.GameControllerBase: dense static range 0x08..0x78 matches dump.cs
// (<LZO>@8, <LZp>@0x10, <LZT>@0x18, <LZR>@0x20, <LZE>@0x28, <LZn>@0x30, <LZD>@0x38, …)
inline constexpr std::uint64_t GAME_CONTROLLER_TYPEINFO_RVA         = 0xD1690C8; // GameControllerBase
inline constexpr std::uint64_t GAME_CONTROLLER_LOCAL_PLAYER_FIELD   = 0x10; // <LZp>k__BackingField
inline constexpr std::uint64_t GAME_CONTROLLER_CAMERA_MANAGER_FIELD = 0x38; // <LZD>k__BackingField
inline constexpr std::uint64_t CAMERA_MANAGER_CAMERA_FIELD          = 0x20; // m_Camera

// Oxide.PlayerManager instance fields (dump.cs — offsets unchanged in the new build:
// worldCameraRoot 0x68, lastTickPosition 0x1C8, lastSavedPosition 0x1D4)
inline constexpr std::uint64_t PLAYER_TRANSFORM = 0x68; // worldCameraRoot
// Prefer lastSavedPosition; lastTickPosition is adjacent at 0x1C8
inline constexpr std::uint64_t PLAYER_POSITION  = 0x1D4; // lastSavedPosition

inline constexpr std::uint64_t IL2CPP_LIST_ITEMS          = 0x10;
inline constexpr std::uint64_t IL2CPP_LIST_SIZE           = 0x18;
inline constexpr std::uint64_t IL2CPP_ARRAY_FIRST_ELEMENT = 0x20;

}

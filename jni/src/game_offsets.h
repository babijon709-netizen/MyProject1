#pragma once
#include <cstdint>

namespace game_offsets {

// Native Unity Camera (via Camera::m_CachedPtr) — from libunity.so (dump2)
// Projection still at 0x140; worldToCamera/view moved (old 0x2F8 is dead on this build).
inline constexpr std::uint64_t CAMERA_PROJECTION_MATRIX = 0x140;
inline constexpr std::uint64_t CAMERA_VIEW_MATRIX       = 0x1F8;

inline constexpr std::uint64_t MANAGED_CACHED_PTR = 0x10;

inline constexpr float PLAYER_HEIGHT          = 1.8F;
inline constexpr float PLAYER_BOX_WIDTH_RATIO = 0.40F;
inline constexpr float MIN_PLAYER_DISTANCE    = 0.0F;
inline constexpr float MAX_PLAYER_DISTANCE    = 300.0F;

// Il2CppClass* TypeInfo globals in libil2cpp.so (from dump.cs / libil2cpp GOT)
inline constexpr std::uint64_t PLAYER_MANAGER_TYPEINFO_RVA       = 0xD48CFB0;
inline constexpr std::uint64_t PLAYER_MANAGER_STATIC_FIELDS_LIST = 0x10; // clientPlayerList

inline constexpr std::uint64_t GAME_CONTROLLER_TYPEINFO_RVA         = 0xD4884E8; // GameControllerBase
inline constexpr std::uint64_t GAME_CONTROLLER_LOCAL_PLAYER_FIELD   = 0x10; // <LZp>k__BackingField
inline constexpr std::uint64_t GAME_CONTROLLER_CAMERA_MANAGER_FIELD = 0x38; // <LZD>k__BackingField
inline constexpr std::uint64_t CAMERA_MANAGER_CAMERA_FIELD          = 0x20; // m_Camera

// Oxide.PlayerManager instance fields (dump.cs)
inline constexpr std::uint64_t PLAYER_TRANSFORM = 0x68; // worldCameraRoot
// Prefer lastSavedPosition; lastTickPosition is adjacent at 0x1C8
inline constexpr std::uint64_t PLAYER_POSITION  = 0x1D4; // lastSavedPosition

inline constexpr std::uint64_t IL2CPP_LIST_ITEMS          = 0x10;
inline constexpr std::uint64_t IL2CPP_LIST_SIZE           = 0x18;
inline constexpr std::uint64_t IL2CPP_ARRAY_FIRST_ELEMENT = 0x20;

}

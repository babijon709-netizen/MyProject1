#pragma once
#include <cstdint>

namespace game_offsets {

// Unity Camera native object offsets (from libunity.so)
inline constexpr std::uint64_t CAMERA_PROJECTION_MATRIX = 0x140;
inline constexpr std::uint64_t CAMERA_VIEW_MATRIX       = 0x2F8;

// Managed to native pointer offset
inline constexpr std::uint64_t MANAGED_CACHED_PTR = 0x10;

// Player visualization constants
inline constexpr float PLAYER_HEIGHT          = 1.8F;
inline constexpr float PLAYER_BOX_WIDTH_RATIO = 0.40F;
inline constexpr float MIN_PLAYER_DISTANCE    = 0.0F;
inline constexpr float MAX_PLAYER_DISTANCE    = 300.0F;

// Oxide.PlayerManager class info (from libil2cpp.so)
// TYPEINFO RVA addresses are in .bss section and filled at runtime
inline constexpr std::uint64_t PLAYER_MANAGER_TYPEINFO_RVA       = 0xD17B000;  // From .cctor analysis
inline constexpr std::uint64_t PLAYER_MANAGER_STATIC_FIELDS_LIST = 0x10;       // clientPlayerList at 0x10

// Oxide.GameControllerBase class info (from libil2cpp.so)
inline constexpr std::uint64_t GAME_CONTROLLER_TYPEINFO_RVA        = 0xD17C000;  // From .cctor analysis
inline constexpr std::uint64_t GAME_CONTROLLER_LOCAL_PLAYER_FIELD  = 0x10;       // <LZp>k__BackingField
inline constexpr std::uint64_t GAME_CONTROLLER_CAMERA_MANAGER_FIELD = 0x38;      // <LZD>k__BackingField
inline constexpr std::uint64_t CAMERA_MANAGER_CAMERA_FIELD          = 0x20;      // m_Camera in Oxide.CameraManager

// Oxide.PlayerManager instance field offsets (from dump.cs)
inline constexpr std::uint64_t PLAYER_TRANSFORM = 0x68;  // worldCameraRoot (UnityEngine.Transform)
inline constexpr std::uint64_t PLAYER_POSITION  = 0x1D4; // lastSavedPosition (UnityEngine.Vector3)

// IL2CPP collections offsets
inline constexpr std::uint64_t IL2CPP_LIST_ITEMS          = 0x10;
inline constexpr std::uint64_t IL2CPP_LIST_SIZE           = 0x18;
inline constexpr std::uint64_t IL2CPP_ARRAY_FIRST_ELEMENT = 0x20;

}

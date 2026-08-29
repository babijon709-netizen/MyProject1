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
// Note: TYPEINFO_RVA values need to be found at runtime or through binary analysis
// The code validates class names at runtime, so these can be 0 if unknown
inline constexpr std::uint64_t PLAYER_MANAGER_TYPEINFO_RVA       = 0xD126870;  // Needs update for new binary
inline constexpr std::uint64_t PLAYER_MANAGER_STATIC_FIELDS_LIST = 0x10;       // clientPlayerList at 0x10 (not activePlayerList at 0x8!)

// Oxide.GameControllerBase class info (from libil2cpp.so)
inline constexpr std::uint64_t GAME_CONTROLLER_TYPEINFO_RVA        = 0xD121CF8;  // Needs update for new binary
inline constexpr std::uint64_t GAME_CONTROLLER_LOCAL_PLAYER_FIELD  = 0x10;       // <LZp>k__BackingField
inline constexpr std::uint64_t GAME_CONTROLLER_CAMERA_MANAGER_FIELD = 0x38;      // <LZD>k__BackingField
inline constexpr std::uint64_t CAMERA_MANAGER_CAMERA_FIELD          = 0x20;      // m_Camera in Oxide.CameraManager

// Oxide.PlayerManager instance field offsets
inline constexpr std::uint64_t PLAYER_TRANSFORM = 0x68;  // worldCameraRoot (UnityEngine.Transform)
inline constexpr std::uint64_t PLAYER_POSITION  = 0x1D0; // Position offset (auto-discovered at runtime)

// IL2CPP collections offsets
inline constexpr std::uint64_t IL2CPP_LIST_ITEMS          = 0x10;
inline constexpr std::uint64_t IL2CPP_LIST_SIZE           = 0x18;
inline constexpr std::uint64_t IL2CPP_ARRAY_FIRST_ELEMENT = 0x20;

}

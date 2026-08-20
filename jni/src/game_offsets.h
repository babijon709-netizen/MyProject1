#pragma once
#include <cstdint>

namespace game_offsets {

inline constexpr std::uint64_t CAMERA_PROJECTION_MATRIX = 0x140;
inline constexpr std::uint64_t CAMERA_VIEW_MATRIX       = 0x2F8;

inline constexpr std::uint64_t MANAGED_CACHED_PTR = 0x10;

inline constexpr float PLAYER_HEIGHT          = 1.8F;
inline constexpr float PLAYER_BOX_WIDTH_RATIO = 0.40F;
inline constexpr float MIN_PLAYER_DISTANCE    = 0.0F;
inline constexpr float MAX_PLAYER_DISTANCE    = 300.0F;

inline constexpr std::uint64_t PLAYER_MANAGER_TYPEINFO_RVA       = 0xD126870;
inline constexpr std::uint64_t PLAYER_MANAGER_STATIC_FIELDS_LIST = 0x10;

inline constexpr std::uint64_t GAME_CONTROLLER_TYPEINFO_RVA        = 0xD121CF8;
inline constexpr std::uint64_t GAME_CONTROLLER_LOCAL_PLAYER_FIELD  = 0x10;
inline constexpr std::uint64_t GAME_CONTROLLER_CAMERA_MANAGER_FIELD = 0x38;
inline constexpr std::uint64_t CAMERA_MANAGER_CAMERA_FIELD          = 0x20;

inline constexpr std::uint64_t PLAYER_TRANSFORM = 0x68;
inline constexpr std::uint64_t PLAYER_POSITION  = 0x1D0;

// Local player aim state (PlayerManager.playerEventHandler = huU : huc).
inline constexpr std::uint64_t PLAYER_EVENT_HANDLER = 0x78;
inline constexpr std::uint64_t HUC_AIM              = 0x268; // huc.Aim (huT)
inline constexpr std::uint64_t HUC_AIM_RAYCAST      = 0x168; // huc.AimRaycast (huW<huz>)
inline constexpr std::uint64_t HUT_STATE            = 0x10;  // huT.TCz (bool)
inline constexpr std::uint64_t HUW_VALUE            = 0x10;  // huW<T>.TUg
inline constexpr std::uint64_t HUZ_HIT              = 0x10;  // huz.Tun (bool)
inline constexpr std::uint64_t HUZ_PLAYER           = 0x40;  // huz.TuV (PlayerManager)

inline constexpr std::uint64_t IL2CPP_LIST_ITEMS          = 0x10;
inline constexpr std::uint64_t IL2CPP_LIST_SIZE           = 0x18;
inline constexpr std::uint64_t IL2CPP_ARRAY_FIRST_ELEMENT = 0x20;

}

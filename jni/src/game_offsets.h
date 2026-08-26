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

// Aim points are exact world-space offsets above the feet, projected through the
// live camera (standing model proportions).
// Fixed values to aim at actual head height instead of above the head.
inline constexpr float BONE_HEAD_HEIGHT   = 1.50F;
inline constexpr float BONE_CHEST_HEIGHT  = 1.20F;
inline constexpr float BONE_PELVIS_HEIGHT = 0.85F;

// Source: dump arm64-v8a 1.13.11888 (Il2CppDumper output: dump.cs / il2cpp.h / script.json)
// PlayerManager (Oxide)   TypeInfo ptr RVA: script.json TypeInfoPointers -> Oxide.PlayerManager
// GameControllerBase (Oxide) TypeInfo ptr RVA: script.json TypeInfoPointers -> Oxide.GameControllerBase
inline constexpr std::uint64_t PLAYER_MANAGER_TYPEINFO_RVA       = 0xD2BBAD8;
inline constexpr std::uint64_t PLAYER_MANAGER_STATIC_FIELDS_LIST = 0x10; // PlayerManager.clientPlayerList

inline constexpr std::uint64_t GAME_CONTROLLER_TYPEINFO_RVA         = 0xD2B6ED8;
inline constexpr std::uint64_t GAME_CONTROLLER_LOCAL_PLAYER_FIELD   = 0x10; // GameControllerBase.<zOu>k__BackingField (PlayerManager)
inline constexpr std::uint64_t GAME_CONTROLLER_CAMERA_MANAGER_FIELD = 0x38; // GameControllerBase.<zOI>k__BackingField (CameraManager)
inline constexpr std::uint64_t CAMERA_MANAGER_CAMERA_FIELD          = 0x20; // CameraManager.m_Camera

// Local input: GameControllerBase.<zOD>k__BackingField -> PlayerInputHandler,
// embedded PlayerInput struct holds the live button states (Aim = ADS held).
inline constexpr std::uint64_t GAME_CONTROLLER_INPUT_HANDLER_FIELD = 0x20; // PlayerInputHandler
inline constexpr std::uint64_t PLAYER_INPUT_STRUCT_OFFSET          = 0x40; // PlayerInputHandler.zkt (embedded)
inline constexpr std::uint64_t PLAYER_INPUT_AIM_OFFSET             = 0x1B; // PlayerInput.Aim

inline constexpr std::uint64_t PLAYER_TRANSFORM = 0x68;  // PlayerManager.worldCameraRoot (Transform)
inline constexpr std::uint64_t PLAYER_POSITION  = 0x1D0; // PlayerManager.lastTickPosition (Vector3)

inline constexpr std::uint64_t IL2CPP_LIST_ITEMS          = 0x10;
inline constexpr std::uint64_t IL2CPP_LIST_SIZE           = 0x18;
inline constexpr std::uint64_t IL2CPP_ARRAY_FIRST_ELEMENT = 0x20;

}

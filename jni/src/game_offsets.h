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

// Aim points are exact world-space offsets above the FEET, projected through the
// live camera (standing model proportions, PLAYER_HEIGHT = 1.8m).
// The entity position field read from memory is NOT assumed to be at feet level:
// esp_get_boxes calibrates its real height above the feet at runtime by comparing
// the local player's position (same field) against the camera eye from the view
// matrix (eye sits ~EYE_ABOVE_FEET over the feet). The measured offset is
// subtracted before these bone heights are applied, so aim stays anatomically
// correct no matter which position field the freshness tuner picks.
inline constexpr float EYE_ABOVE_FEET     = 1.60F; // camera eye height over the feet
inline constexpr float BONE_HEAD_HEIGHT   = 1.58F; // face/neck - reliable head hit
inline constexpr float BONE_CHEST_HEIGHT  = 1.20F; // center of the torso
inline constexpr float BONE_PELVIS_HEIGHT = 0.85F; // pelvis/upper legs

// Real skeleton bones. PlayerModelInfo holds the third-person model's head/body
// Transforms; reading their live world positions through the transform hierarchy
// follows every pose - crouching, leaning, jumping - unlike fixed offsets above
// the feet. The constant heights above stay as the fallback when the model is
// not loaded (sleepers, streaming) or the read fails validation.
// Chain (dump arm64-v8a 1.13.11888):
//   PlayerManager.inventory (0x98, PlayerInventory)
//     -> _playerInventoryData (0x20, PlayerInventoryData; .player 0x10 backref)
//       -> playerModelInfo (0x20, PlayerModelInfo)
//         -> head (0x20, Transform), body (0x40, Transform)
inline constexpr std::uint64_t PLAYER_INVENTORY_FIELD       = 0x98; // PlayerManager.inventory
inline constexpr std::uint64_t PLAYER_INVENTORY_DATA_FIELD  = 0x20; // PlayerInventory._playerInventoryData
inline constexpr std::uint64_t INVENTORY_DATA_PLAYER_FIELD  = 0x10; // PlayerInventoryData.player (backref)
inline constexpr std::uint64_t INVENTORY_DATA_MODEL_FIELD   = 0x20; // PlayerInventoryData.playerModelInfo
inline constexpr std::uint64_t MODEL_INFO_HEAD_FIELD        = 0x20; // PlayerModelInfo.head (Transform)
inline constexpr std::uint64_t MODEL_INFO_BODY_FIELD        = 0x40; // PlayerModelInfo.body (Transform)

inline constexpr float HEAD_BONE_CENTER_LIFT = 0.07F; // head pivot sits at the neck joint; lift to skull center
inline constexpr float HEAD_TOP_MARGIN       = 0.18F; // skull top above the head pivot (for box height)
inline constexpr float CHEST_FRACTION        = 0.70F; // chest at this fraction of feet->head height
inline constexpr float PELVIS_FRACTION       = 0.45F; // pelvis at this fraction of feet->head height

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

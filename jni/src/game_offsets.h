#pragma once
#include <cstdint>

namespace game_offsets {

// --- Native UnityEngine.Camera (libunity.so, Unity 6000.3.18f1, arm64) -------
// Layout proven by disassembling the native camera icalls in libunity.so:
//   Camera::GetWorldToCameraMatrix_Injected (0x5ac588) / WorldToScreenPoint
//   internals (0xe304cc) read the view matrix from            +0x70
//   Camera::GetProjectionMatrix_Injected  (0x5ac5b0 -> 0xe1fef4)
//     computes and caches the projection matrix at              +0xB0
//   Camera::SetProjectionMatrix_Injected  (0x5ac5d4 -> 0xe20868)
//     writes +0xB0 and, when the matrix is a clean frustum (columns 2 zero),
//     also the untouched copy - native m_OriginalProjectionMatrix - at +0x130.
//     Oblique/jittered projections only ever land in +0xB0, so +0x130 keeps
//     the original matrix. SetNonJitteredProjectionMatrix uses +0x748.
//   Camera::GetProjectionMatrix uses the float at              +0x40
//     as the field-of-view input (native m_FieldOfView).
// STORAGE ORDER: the native buffers are unity::math::Matrix4x4f (row-major
// math). The GL-style frustum's w-row z coefficient (-1 at math (3,2)) lands
// at mem[14] row-major / mem[11] column-major, and the reader verifies the
// order from that marker per buffer (transposing the rare column-major case).
// VIEW SOURCE: the engine's own WorldToScreenPoint computes the view FRESH
// from the camera transform (its +0x70 managed-facing cache can stay identity
// when no C# touches worldToCameraMatrix), so the reader resolves the camera's
// native Transform* inside the camera object (hierarchy-validated, pinned near
// the player's eye) and builds the view from its pose; +0x70 (only if not
// identity) and the local player's worldCameraRoot follow as fallbacks.
// SELECTION: +0xB0 is what the engine itself uses for WorldToScreenPoint, so
// it is primary; +0x748 and +0x130 are alternates picked by best FOV match
// (they can go stale while the game lerps fieldOfView); if every cached
// candidate is invalid the projection is rebuilt from +0x40 + screen aspect.
inline constexpr std::uint64_t NATIVE_CAMERA_VIEW_MATRIX             = 0x70;
inline constexpr std::uint64_t NATIVE_CAMERA_PROJECTION_MATRIX       = 0xB0;
inline constexpr std::uint64_t NATIVE_CAMERA_ORIGINAL_PROJECTION     = 0x130;
inline constexpr std::uint64_t NATIVE_CAMERA_NON_JITTERED_PROJECTION = 0x748;
inline constexpr std::uint64_t NATIVE_CAMERA_FOV                     = 0x40;

// Managed UnityEngine.Object.m_CachedPtr (dump.cs: UnityEngine.CoreModule,
// class Object, "private IntPtr m_CachedPtr; // 0x10").
inline constexpr std::uint64_t MANAGED_CACHED_PTR = 0x10;

inline constexpr float PLAYER_HEIGHT          = 1.8F;
inline constexpr float PLAYER_BOX_WIDTH_RATIO = 0.40F;
inline constexpr float MIN_PLAYER_DISTANCE    = 0.0F;
inline constexpr float MAX_PLAYER_DISTANCE    = 300.0F;

// Aim points are exact world-space offsets above the FEET, projected through
// the live camera (standing model proportions, PLAYER_HEIGHT = 1.8m).
// The position field read from memory (lastTickPosition) sits at feet level.
inline constexpr float BONE_HEAD_HEIGHT   = 1.58F; // face/neck - reliable head hit
inline constexpr float BONE_CHEST_HEIGHT  = 1.20F; // center of the torso
inline constexpr float BONE_PELVIS_HEIGHT = 0.85F; // pelvis/upper legs

// Real skeleton bones. PlayerModelInfo holds the third-person model's head/body
// Transforms; reading their live world positions through the transform hierarchy
// follows every pose - crouching, leaning, jumping - unlike fixed offsets above
// the feet. The constant heights above stay as the fallback when the model is
// not loaded (sleepers, streaming) or the read fails validation.
// Chain (dump arm64-v8a 1.13.11888, dump.cs):
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

// --- libil2cpp.so offsets (dump arm64-v8a 1.13.11888: dump.cs / script.json) --
// TypeInfo pointer RVAs (script.json TypeInfoPointers):
//   0xD2BBAD8 Oxide_PlayerManager_TypeInfo     -> Oxide.PlayerManager
//   0xD2B6ED8 Oxide_GameControllerBase_TypeInfo -> Oxide.GameControllerBase
inline constexpr std::uint64_t PLAYER_MANAGER_TYPEINFO_RVA       = 0xD2BBAD8;
inline constexpr std::uint64_t PLAYER_MANAGER_STATIC_FIELDS_LIST = 0x10; // PlayerManager.clientPlayerList

inline constexpr std::uint64_t GAME_CONTROLLER_TYPEINFO_RVA         = 0xD2B6ED8;
inline constexpr std::uint64_t GAME_CONTROLLER_LOCAL_PLAYER_FIELD   = 0x10; // GameControllerBase.<zOu>k__BackingField (PlayerManager)
inline constexpr std::uint64_t GAME_CONTROLLER_CAMERA_MANAGER_FIELD = 0x38; // GameControllerBase.<zOI>k__BackingField (CameraManager)
inline constexpr std::uint64_t CAMERA_MANAGER_CAMERA_FIELD          = 0x20; // CameraManager.m_Camera

// Player fields (dump.cs, class PlayerManager):
inline constexpr std::uint64_t PLAYER_TRANSFORM = 0x68;  // PlayerManager.worldCameraRoot (Transform)
inline constexpr std::uint64_t PLAYER_POSITION  = 0x1D0; // PlayerManager.lastTickPosition (Vector3, feet level)

// ADS state (dump.cs): PlayerManager.fpManager (0x90, FPManager) ->
// _currentWeapon (0x50, FPObject) -> normalFOV (0x80) / aimFOV (0x84).
// The camera is zoomed toward aimFOV while aiming, so comparing the live
// native camera FOV against the midpoint of the two values detects ADS.
inline constexpr std::uint64_t PLAYER_FP_MANAGER_FIELD            = 0x90; // PlayerManager.fpManager
inline constexpr std::uint64_t FP_MANAGER_CURRENT_WEAPON_FIELD    = 0x50; // FPManager._currentWeapon (FPObject)
inline constexpr std::uint64_t FP_OBJECT_NORMAL_FOV_FIELD         = 0x80; // FPObject.normalFOV (int)
inline constexpr std::uint64_t FP_OBJECT_AIM_FOV_FIELD            = 0x84; // FPObject.aimFOV (int)

inline constexpr std::uint64_t IL2CPP_LIST_ITEMS          = 0x10;
inline constexpr std::uint64_t IL2CPP_LIST_SIZE           = 0x18;
inline constexpr std::uint64_t IL2CPP_ARRAY_FIRST_ELEMENT = 0x20;

}

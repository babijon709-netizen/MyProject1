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

// PlayerManager.worldCameraRoot is a managed Transform and is also a useful
// entry point into the player's native Transform hierarchy.
inline constexpr std::uint64_t PLAYER_TRANSFORM       = 0x68;
inline constexpr std::uint64_t PLAYER_KCC_REFERENCE   = 0xB0;
inline constexpr std::uint64_t PLAYER_VITALS          = 0xC8;
inline constexpr std::uint64_t PLAYER_WEAPON_REFERENCE = 0xF0;
inline constexpr std::uint64_t PLAYER_NICKLABEL       = 0x130;
inline constexpr std::uint64_t PLAYER_CHARACTER_MODEL = 0x150;
inline constexpr std::uint64_t PLAYER_ANIMATOR        = 0x198;
inline constexpr std::uint64_t PLAYER_POSITION        = 0x1D0;
inline constexpr std::uint64_t PLAYER_USER_ID         = 0x280;

// wK (the world-space nickname label) -> UnityEngine.UI.Text -> m_Text.
inline constexpr std::uint64_t NICKLABEL_TEXT       = 0x38;
inline constexpr std::uint64_t UI_TEXT_VALUE        = 0xE0;

// huU inherits huC. huC.Health is huo<float, ReasonedValueReasonHealthEvent>;
// its current value is the first float payload at +0x20.
inline constexpr std::uint64_t HUC_HEALTH           = 0x98;
inline constexpr std::uint64_t HUO_CURRENT_FLOAT    = 0x20;
inline constexpr std::uint64_t PLAYER_VITALS_MAX_HP = 0x88;

// InterfaceReference<HR> and PlayerWeapon. WeaponPiece.Number is a short at
// PlayerWeapon+0xCA (WeaponPiece starts at +0xC8).
inline constexpr std::uint64_t INTERFACE_REFERENCE_VALUE = 0x18;
inline constexpr std::uint64_t PLAYER_WEAPON_NUMBER      = 0xCA;

// KCC (the character controller behind PlayerManager.kccReference). Verified
// against the il2cpp dump: KCC.hitBoxRecorderRoot 0x70, KCC.player 0x78,
// KCC.head 0x88, KCC.<Pose> 0x274, KCC.<MoveState> 0x278.
inline constexpr std::uint64_t KCC_PLAYER                     = 0x78;
inline constexpr std::uint64_t KCC_HITBOX_ROOT                = 0x70;
inline constexpr std::uint64_t KCC_HEAD_TRANSFORM             = 0x88;
inline constexpr std::uint64_t KCC_POSE                       = 0x274; // 0 stand, 1 crouch
inline constexpr std::uint64_t KCC_MOVE_STATE                 = 0x278; // MoveState (byte)
inline constexpr std::uint64_t KCC_CHARACTER_ANIMATION        = 0x108;
inline constexpr std::uint64_t CHARACTER_ANIMATION_BONE_CACHE = 0x88;
inline constexpr std::uint64_t BONE_CACHE_TRANSFORMS           = 0x48;
inline constexpr std::uint64_t BONE_CACHE_MAPPING              = 0x50;

// CharacterAnimation.playerModelInfo -> PlayerModelInfo.head / .body.
// Verified in dump3.zip: PlayerModelInfo.head is 0x20 and body is 0x40.
inline constexpr std::uint64_t CHARACTER_ANIMATION_MODEL_INFO = 0x30;
inline constexpr std::uint64_t MODEL_INFO_HEAD                = 0x20;
inline constexpr std::uint64_t MODEL_INFO_BODY                = 0x40;

// HitBoxRecorderRoot.hitBoxes is a HitBox[]; every HitBox is a MonoBehaviour
// sitting on (or directly under) a bone of the animated character, and
// HitBox.m_HitArea tells which body part that bone belongs to.
inline constexpr std::uint64_t HITBOX_ROOT_BOXES              = 0x68;
inline constexpr std::uint64_t HITBOX_HIT_AREA                = 0x68;
// HitArea enum values.
inline constexpr int HIT_AREA_ALL   = 0;
inline constexpr int HIT_AREA_HEAD  = 1;
inline constexpr int HIT_AREA_CHEST = 2;
inline constexpr int HIT_AREA_LEG   = 3;
inline constexpr int HIT_AREA_FOOT  = 4;
inline constexpr int HIT_AREA_HAND  = 5;
// MoveState.CROUCHING
inline constexpr int MOVE_STATE_CROUCHING = 3;

// Managed offsets verified against dump3.zip (dump.cs, 2026-08-18).
// Native Unity Transform/GameObject offsets below are not present in the
// managed dump and therefore must not be treated as dump-verified.
inline constexpr std::uint64_t NATIVE_COMPONENT_GAME_OBJECT = 0x30;
inline constexpr std::uint64_t NATIVE_GAME_OBJECT_COMPONENTS = 0x30;
inline constexpr std::uint64_t NATIVE_GAME_OBJECT_NAME       = 0x60;
inline constexpr std::uint64_t NATIVE_TRANSFORM_CHILDREN     = 0x70;
inline constexpr std::uint64_t NATIVE_TRANSFORM_CHILD_COUNT  = 0x80;
inline constexpr std::uint64_t NATIVE_TRANSFORM_PARENT       = 0x90;

// Death / spectate detection (local PlayerManager). Spectating is detected via
// observedPlayer (the back-reference to the player being followed) instead of a
// PlayerFlags bit, whose numeric value is not reliable across builds.
inline constexpr std::uint64_t PLAYER_RESPAWNING     = 0x210; // bool
inline constexpr std::uint64_t PLAYER_OBSERVED       = 0x320; // PlayerManager observedPlayer

// Local player aim state (PlayerManager.playerEventHandler = huU : huc).
inline constexpr std::uint64_t PLAYER_EVENT_HANDLER = 0x78;
inline constexpr std::uint64_t HUC_BOUNDS           = 0x70;  // huc.bounds (Unity Bounds: center+extents)
inline constexpr std::uint64_t HUC_AIM              = 0x268; // huc.Aim (huT)
inline constexpr std::uint64_t HUC_CROUCH           = 0x258; // huU.Crouch (huT)
inline constexpr std::uint64_t HUC_AIM_RAYCAST      = 0x168; // huU.AimRaycast (huW<huz>)
inline constexpr std::uint64_t HUC_RAYCAST_DATA     = 0x160; // huU.RaycastData (huW<huz>)
inline constexpr std::uint64_t HUC_VELOCITY         = 0xB0;  // huC.Velocity (huW<Vector3>)
inline constexpr std::uint64_t HUT_STATE            = 0x10;  // huT.TCz (bool)
inline constexpr std::uint64_t HUW_VALUE            = 0x20;  // huW<T>.TUg (current value)
inline constexpr std::uint64_t HUW_PREV             = 0x28;  // huW<T>.TUm (previous value)
inline constexpr std::uint64_t HUZ_HIT              = 0x10;  // huz.Tun (bool)
inline constexpr std::uint64_t HUZ_PLAYER           = 0x40;  // huz.TuV (PlayerManager)

inline constexpr std::uint64_t IL2CPP_LIST_ITEMS          = 0x10;
inline constexpr std::uint64_t IL2CPP_LIST_SIZE           = 0x18;
inline constexpr std::uint64_t IL2CPP_ARRAY_FIRST_ELEMENT = 0x20;

}

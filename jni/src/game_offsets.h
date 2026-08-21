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

// The player owns the live KCC/model graph through an InterfaceReference<ti>.
// These fields are intentionally kept separate from the network position above:
// network position is useful for the box, while the KCC hierarchy contains the
// animated bones rendered by the client.
inline constexpr std::uint64_t PLAYER_KCC_REFERENCE = 0xB0;
inline constexpr std::uint64_t KCC_PLAYER           = 0x78;
inline constexpr std::uint64_t KCC_ANIMATOR          = 0xD8;
inline constexpr std::uint64_t KCC_CHARACTER_ANIMATION = 0x108;
inline constexpr std::uint64_t CHARACTER_ANIMATION_ANIMATOR = 0x20;

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

// ---------------------------------------------------------------------------
// Skeleton (bone) ESP.
//
// The enemy model is a Unity humanoid: PlayerManager carries its Animator
// directly (`animator`, confirmed from the dump).  Unity's Animator stores an
// Avatar, and the Avatar holds the humanoid rig mapping (HumanBodyBones ->
// transform index).  We read that mapping and resolve each bone's world
// position through the same Transform hierarchy code the ESP already uses.
//
// The chain is:
//   PlayerManager.animator (managed Animator*)        -> m_CachedPtr -> native Animator
//   native Animator + ANIMATOR_AVATAR                 -> native Avatar
//   native Avatar   + AVATAR_DATA                     -> AvatarData (serialized rig)
//   native Avatar   + AVATAR_SIZE                     -> uint32 bone/node count
//   AvatarData      + AVATAR_DATA_TOS                 -> TransformOffsetStructure[] (skeleton nodes)
//   AvatarData      + AVATAR_DATA_HUMAN_BONE_INDEX    -> uint32[HumanBodyBones] (node index per bone)
//   TransformOffsetStructure[bone].m_Index            -> transform hierarchy index
//
// NOTE: the Animator/Avatar/AvatarData native offsets below are engine-internal
// and are discovered + validated at runtime (see game.cpp discover_bone_layout),
// so a wrong default simply falls back to scanning instead of drawing garbage.
// ---------------------------------------------------------------------------
inline constexpr std::uint64_t PLAYER_ANIMATOR = 0x198; // PlayerManager.animator

inline constexpr std::uint64_t ANIMATOR_AVATAR          = 0x38; // native Animator.m_Avatar
inline constexpr std::uint64_t AVATAR_DATA              = 0x28; // native Avatar.m_Avatar (AvatarData*)
inline constexpr std::uint64_t AVATAR_SIZE              = 0x30; // native Avatar.m_AvatarSize
inline constexpr std::uint64_t AVATAR_DATA_TOS          = 0x10; // AvatarData.m_TOS (TransformOffsetStructure[])
inline constexpr std::uint64_t AVATAR_DATA_HUMAN_BONE_INDEX = 0x38; // AvatarData.m_HumanBoneIndex (uint32[])
inline constexpr std::uint64_t TOS_STRIDE              = 0x18; // sizeof(TransformOffsetStructure)
inline constexpr std::uint64_t TOS_COUNT_OFF           = 0x08; // TransformOffsetStructure.m_Count
inline constexpr std::uint64_t TOS_FIRST_INDEX_OFF     = 0x0C; // TransformOffsetStructure.m_Index

}

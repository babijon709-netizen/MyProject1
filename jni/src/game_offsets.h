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
inline constexpr std::uint64_t PLAYER_CHARACTER_MODEL = 0x150;
inline constexpr std::uint64_t PLAYER_ANIMATOR        = 0x198;
inline constexpr std::uint64_t PLAYER_KCC_REFERENCE   = 0xB0;
inline constexpr std::uint64_t PLAYER_VIEW_QL         = 0x248;

inline constexpr std::uint64_t INTERFACE_COMPONENT = 0x10;
inline constexpr std::uint64_t INTERFACE_VALUE     = 0x18;

inline constexpr std::uint64_t KCC_HITBOX_ROOT        = 0x70;
inline constexpr std::uint64_t KCC_PLAYER             = 0x78;
inline constexpr std::uint64_t SINGLE_KCC_HITBOX_ROOT = 0x78;
inline constexpr std::uint64_t SINGLE_KCC_PLAYER      = 0x80;

inline constexpr std::uint64_t HITBOX_ROOT_BOXES     = 0x68;
inline constexpr std::uint64_t HITBOX_ROOT_RECORDERS = 0x80;
inline constexpr std::uint64_t HITBOX_AREA           = 0x68;
inline constexpr std::uint64_t HITBOX_BOUNDS_CENTER  = 0x88;
inline constexpr std::uint64_t QA_CAPTURES           = 0x20;
inline constexpr std::uint64_t QA_WRITE_INDEX        = 0x28;
inline constexpr std::uint64_t QA_POSITION           = 0x34;
inline constexpr std::uint64_t CAPTURE_STRIDE        = 0x28;

inline constexpr std::uint64_t QL_PLAYER_MODEL_INFO = 0x10;

inline constexpr std::uint64_t PMI_HEAD         = 0x20;
inline constexpr std::uint64_t PMI_RIGHT_WEAPON = 0x28;
inline constexpr std::uint64_t PMI_LEFT_WEAPON  = 0x30;
inline constexpr std::uint64_t PMI_EQUIPMENT    = 0x38;
inline constexpr std::uint64_t PMI_BODY         = 0x40;
inline constexpr std::uint64_t PMI_HEAD_MODEL   = 0x48;
inline constexpr std::uint64_t PMI_SKINNED_MESH = 0x50;
inline constexpr std::uint64_t PMI_CHARACTER_ANIM = 0x60;

inline constexpr std::uint64_t CHAR_ANIM_PLAYER_MODEL = 0x30;
inline constexpr std::uint64_t CHAR_ANIM_BONE_CONFIGS = 0x40;
inline constexpr std::uint64_t CHAR_ANIM_BONE_DRIVER  = 0x88;
inline constexpr std::uint64_t BONE_DRIVER_TRANSFORMS = 0x48;
inline constexpr std::uint64_t BONE_DRIVER_HUMAN_IDS  = 0x50;
inline constexpr std::uint64_t HUMAN_BONE_CONFIG_BONES = 0x28;
inline constexpr std::uint64_t HUMAN_BONE_ID          = 0x10;

inline constexpr std::uint64_t IL2CPP_LIST_ITEMS          = 0x10;
inline constexpr std::uint64_t IL2CPP_LIST_SIZE           = 0x18;
inline constexpr std::uint64_t IL2CPP_ARRAY_FIRST_ELEMENT = 0x20;

}

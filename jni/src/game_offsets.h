#pragma once
#include <cstdint>

namespace game_offsets {

// Native Unity Camera (via Camera::m_CachedPtr) — from libunity.so (dump2)
// get_worldToCameraMatrix_Injected @ 0x5ac514 → helper 0xe1fe08 returns cam+0x70
// get_projectionMatrix_Injected    @ 0x5ac53c → helper 0xe1fe6c returns cam+0xB0
// IMPORTANT: +0x70 / +0xB0 are LAZY caches (dirty flags @ +0x502 / +0x500).
// External process_vm_readv does NOT run the getters, so +0x70 goes stale when the
// camera moves — that is the ESP "drifts with camera" bug. Prefer rebuilding view
// from the live Transform at cam+0x20 (same pointer the dirty path uses).
inline constexpr std::uint64_t CAMERA_PROJECTION_MATRIX = 0xB0;  // Matrix4x4 projection (cached)
inline constexpr std::uint64_t CAMERA_VIEW_MATRIX       = 0x70;  // Matrix4x4 worldToCamera (cached, stale!)
inline constexpr std::uint64_t CAMERA_WORLD_TO_CLIP     = 0xF0;  // projection * worldToCamera product
inline constexpr std::uint64_t CAMERA_PREV_VIEW_PROJ    = 0x5C8; // previousViewProjection (frame-written)
inline constexpr std::uint64_t CAMERA_NATIVE_TRANSFORM  = 0x20;  // Transform* used by w2c dirty rebuild
inline constexpr std::uint64_t CAMERA_FOV_DEGREES       = 0x170; // get_fieldOfView storage
inline constexpr std::uint64_t CAMERA_ASPECT            = 0x4E0;
inline constexpr std::uint64_t CAMERA_NEAR_CLIP         = 0x454;
inline constexpr std::uint64_t CAMERA_FAR_CLIP          = 0x458;
inline constexpr std::uint64_t CAMERA_VIEW_DIRTY        = 0x502; // byte, set when w2c cache invalid
inline constexpr std::uint64_t CAMERA_PROJ_DIRTY        = 0x500; // byte, set when proj cache invalid

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
inline constexpr std::uint64_t PLAYER_POSITION  = 0x1D0; // lastSavedPosition
inline constexpr std::uint64_t PLAYER_CHARACTER_MODEL = 0x150; // characterModel (UnityEngine.GameObject)

// Ragdoll bone list route (dump.cs) — game-maintained list of rig bone
// transforms, no name matching needed:
//   PlayerManager.kccReference (0xB0, possibly a wrapper) -> KCC
//   KCC.ZMl = CharacterAnimation (0x108); KCC.player back-ref (0x78) validates
//   CharacterAnimation.ragdoll (0x38); CharacterAnimation.ZCu back-ref (0x78)
//   Ragdoll.m_Pelvis Rigidbody (0x20), Ragdoll.m_Bones BodyPart[] (0x88)
//   Ragdoll.BodyPart.transform (0x10)
inline constexpr std::uint64_t PLAYER_KCC_REFERENCE        = 0xB0;
inline constexpr std::uint64_t KCC_PLAYER_BACKREF          = 0x78;
inline constexpr std::uint64_t KCC_HEAD_TRANSFORM          = 0x88; // KCC.head (managed UnityEngine.Transform)
inline constexpr std::uint64_t KCC_CHARACTER_ANIMATION     = 0x108;
inline constexpr std::uint64_t CHAR_ANIM_PLAYER_BACKREF    = 0x78;
inline constexpr std::uint64_t CHAR_ANIM_RAGDOLL           = 0x38;
inline constexpr std::uint64_t RAGDOLL_PELVIS_RIGIDBODY    = 0x20;
inline constexpr std::uint64_t RAGDOLL_BONES_ARRAY         = 0x88;
inline constexpr std::uint64_t RAGDOLL_BODYPART_TRANSFORM  = 0x10;
inline constexpr std::uint64_t IL2CPP_ARRAY_LENGTH         = 0x18;

// Native Unity object layout — reversed from libunity.so in this repo:
//   Transform::get_childCount_Injected -> ldr w0, [x0, #0x58]
//   Transform::GetChild helper         -> ldr x8, [x0, #0x48]; ldr x0, [x8, w1, uxtw #3]
//   Component::get_gameObject_Injected -> ldr x0, [x0, #0x20]
//   GameObject::get_transform_Injected -> ldr x8, [x0, #0x20]; ldr x19, [x8, #8]
inline constexpr std::uint64_t TRANSFORM_CHILDREN_ARRAY   = 0x48; // Transform** (direct pointers)
inline constexpr std::uint64_t TRANSFORM_CHILD_COUNT      = 0x58; // int32
inline constexpr std::uint64_t COMPONENT_GAMEOBJECT       = 0x20; // native Component -> native GameObject*
inline constexpr std::uint64_t GAMEOBJECT_COMPONENT_ARRAY = 0x20; // native GameObject -> ComponentPair*
inline constexpr std::uint64_t COMPONENT_PAIR_PTR         = 0x08; // pair[0] + 8 == Transform* (first component)
// GameObject name is a 32-byte core::string (SSO): flags byte at +0x1F,
// heap pointer at +0x0 when (flags >= 0x40), inline chars otherwise.
// The exact field offset inside GameObject is discovered at runtime
// (validated against known bone names); 0x48 is the expected value.
inline constexpr std::uint64_t GAMEOBJECT_NAME_GUESS      = 0x48;

inline constexpr std::uint64_t IL2CPP_LIST_ITEMS          = 0x10;
inline constexpr std::uint64_t IL2CPP_LIST_SIZE           = 0x18;
inline constexpr std::uint64_t IL2CPP_ARRAY_FIRST_ELEMENT = 0x20;

}

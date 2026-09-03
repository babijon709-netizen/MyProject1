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
inline constexpr std::uint64_t PLAYER_NICKLABEL = 0x130; // nicklabel (wK MonoBehaviour)
// PlayerManager "LLI" (0x220): the real human-readable display name (confirmed
// on-device — equals the nicklabel widget's private name string, e.g.
// "пахановский" / "#Фришка"), whereas userID/voice names carry the machine id.
inline constexpr std::uint64_t PLAYER_DISPLAY_NAME = 0x220;

// wK (nicklabel): player back-ref + nickname UI.Text.
inline constexpr std::uint64_t NICKLABEL_PLAYER_BACKREF = 0x20;
inline constexpr std::uint64_t NICKLABEL_NICKNAME_TEXT  = 0x38;
// UnityEngine.UI.Text: managed string with the visible nickname.
inline constexpr std::uint64_t UI_TEXT_MTEXT = 0xE0;
// FPObject (base of FPWeaponBase): display name of the held weapon.
inline constexpr std::uint64_t FPOBJECT_OBJECT_NAME = 0x78;
// Dissonance voice identity: reliable synced display-name source.
// PlayerManager.LLT (VoicePlayerState) -> <Name>k__BackingField.
inline constexpr std::uint64_t PLAYER_VOICE_STATE  = 0x2E8;
inline constexpr std::uint64_t VOICE_STATE_NAME     = 0x38;
// PlayerManager.voicePlayer (fuI tracker) -> HvI display string.
inline constexpr std::uint64_t PLAYER_VOICE_PLAYER = 0x140;
inline constexpr std::uint64_t VOICE_PLAYER_TAG     = 0x78;
// FPObject -> Oxide.Item -> Oxide.ItemData display strings.
inline constexpr std::uint64_t FPOBJECT_ITEM        = 0x40; // <Ltl>k__BackingField
inline constexpr std::uint64_t ITEM_DATA            = 0x20; // <LIN>k__BackingField
inline constexpr std::uint64_t ITEMDATA_NAME        = 0x18; // m_Name
inline constexpr std::uint64_t ITEMDATA_SHORTNAME   = 0x20; // m_ShortName
// Il2Cpp System.String layout: int32 length @0x10, UTF-16 chars @0x14.
inline constexpr std::uint64_t IL2CPP_STRING_LENGTH = 0x10;
inline constexpr std::uint64_t IL2CPP_STRING_CHARS  = 0x14;

// Local player "is aiming" (ADS) state (dump.cs + libil2cpp disasm):
//   PlayerManager.playerEventHandler (0x78) -> fvp (player event handler)
//   fvp.manager (0xD0) back-ref == PlayerManager (validation)
//   fvp.Aim (0x268) -> fvT (toggle activity), fvT.<LxG>k__BackingField (0x10) == Active
//   This exact byte is what FPManager.LateUpdate reads to blend the camera to aimFOV.
// Fallback: PlayerManager.fpManager (0x90) -> FPManager.LtZ (0x58, current FPWeaponBase)
//   FPObject.Player (0xC0) back-ref == PlayerManager (validation)
//   FPWeaponBase.<LKk>k__BackingField (0x120) == isAiming (set in weapon StartAim)
//   FPManager.<LtX>k__BackingField (0xA8) == aim blend 0..1 (secondary hint)
inline constexpr std::uint64_t PLAYER_EVENT_HANDLER          = 0x78;
inline constexpr std::uint64_t EVENT_HANDLER_MANAGER_BACKREF = 0xD0;
inline constexpr std::uint64_t EVENT_HANDLER_AIM_ACTIVITY    = 0x268;
// Remote-player held-weapon candidates (dump.cs Oxide.PlayerManager). The FP
// manager (0x90) is a local MonoBehaviour and is often empty for other players,
// so these are the synced/inventory-backed fallbacks to probe:
//   0x98  inventory            (Oxide.PlayerInventory)
//   0xF0  weaponReference      (private; likely the current remote weapon)
//   0x198 weapons              (private array of weapon objects)
inline constexpr std::uint64_t PLAYER_INVENTORY         = 0x98;
inline constexpr std::uint64_t PLAYER_WEAPON_REFERENCE  = 0xF0;
inline constexpr std::uint64_t PLAYER_WEAPONS_ARRAY     = 0x198;
// Oxide.PlayerInventory instance fields (dump.cs).
inline constexpr std::uint64_t INV_PLAYER_INVENTORY_DATA = 0x20; // _playerInventoryData
inline constexpr std::uint64_t INV_PLAYER_INVENTORY_CLIENT = 0x28; // _playerInventoryClient (fmh)
// fvp.LookDirection (0x140): synced Vector3 wrapper, current value at +0x20.
// MouseLook.Update writes m_LookRoot.forward into it every frame and
// FPHitscan casts its hit ray along it (not along the camera transform).
inline constexpr std::uint64_t EVENT_HANDLER_LOOK_DIRECTION  = 0x140;
inline constexpr std::uint64_t SYNC_VALUE_OFFSET             = 0x20;
inline constexpr std::uint64_t ACTIVITY_ACTIVE_FLAG          = 0x10;
inline constexpr std::uint64_t PLAYER_FP_MANAGER             = 0x90;
inline constexpr std::uint64_t FPMANAGER_CURRENT_WEAPON      = 0x58;
inline constexpr std::uint64_t FPMANAGER_CURRENT_OBJECT      = 0x50; // _currentWeapon (FPObject fallback)
inline constexpr std::uint64_t FPMANAGER_AIM_BLEND           = 0xA8;
inline constexpr std::uint64_t FPOBJECT_PLAYER_BACKREF       = 0xC0;
inline constexpr std::uint64_t FPWEAPON_IS_AIMING            = 0x120;

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
inline constexpr std::uint64_t KCC_NORMAL_HEIGHT          = 0xA0; // float, capsule height standing
inline constexpr std::uint64_t KCC_CROUCH_HEIGHT          = 0xA4; // float, capsule height crouched
// Server-side hit volumes: KCC.hitBoxRecorderRoot (0x70) -> HitBoxRecorderRoot
//   .hitBoxes (0x68, Oxide.HitBox[]) ; HitBox.size 0x24 / center 0x30 (local
//   Vector3), m_HitArea 0x68 (0 Head, 1 Chest, 2 Leg, 3 Foot, 4 Hand).
//   World centre == HitBox.transform.TransformPoint(center) (HitBox.Kbr).
inline constexpr std::uint64_t KCC_HITBOX_ROOT            = 0x70;
inline constexpr std::uint64_t HITBOX_ROOT_ARRAY          = 0x68;
inline constexpr std::uint64_t HITBOX_SIZE                = 0x24;
inline constexpr std::uint64_t HITBOX_CENTER              = 0x30;
inline constexpr std::uint64_t HITBOX_AREA                = 0x68;
inline constexpr std::uint64_t KCC_LOOK_HEIGHT_OFFSET     = 0x90; // float, eye = pos + (capsule height + this) * up
// KCC.ZYW : HyperHug.Games.Oxide.Features.Player.Move (value struct @0x16C)
//   +0x00 MoveState State (0 idle,1 walk,2 run,3 crouching,4 air,5 climb,6 swim,7 dead)
//   +0x04 Pose (0 Stand, 1 Crouch)   +0x08 bool Aim  +0x0C Vector3 Position
//   +0x18 Vector3 RealVelocity
inline constexpr std::uint64_t KCC_MOVE                   = 0x16C;
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

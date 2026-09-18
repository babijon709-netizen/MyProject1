#pragma once
#include <cstdint>

// ВНИМАНИЕ: это оффсеты РЕЛИЗНОЙ версии игры — эталон и источник значений для
// переключателя версий. Бета-версия живёт отдельным файлом
// (game_offsets_beta.h, собирается tools/offsets/beta_offsets.py из
// dump_beta.7z), а код читает память через активный набор
// (game_offsets_active.h, выбранная версия — go::SelectBuild). Значения здесь
// правит update_offsets.py; файл беты он не трогает.

namespace game_offsets {

// NOTE: every obfuscated class/field name quoted in the comments below is
// valid only for the dump it was read from - the obfuscator re-rolls them on
// each game build (fvp->pmi, wK->ij, Mo->sR, ...). Names are kept purely as
// breadcrumbs; when re-checking after an update, match structs and fields
// POSITIONALLY (offset + type) with tools/offsets/il2cpp_layout.py.

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
inline constexpr std::uint64_t CAMERA_NATIVE_TRANSFORM  = 0x20;  // Camera (наследник Component) + 0x20 —
                                                                 // GameObject камеры, НЕ Transform. Проверено
                                                                 // по libunity.so релиза: в коде пересборки
                                                                 // матрицы вида (strb wzr,[x19,#0x502] /
                                                                 // stp q0,q1,[x19,#0x70]) значение camera+0x20
                                                                 // подаётся в GetComponentFastPath — то есть
                                                                 // это GameObject. Transform камеры достаём из
                                                                 // его массива компонентов (game.cpp).
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

// Il2CppClass* TypeInfo globals in libil2cpp.so (from dump.cs / libil2cpp GOT).
// These are .data slots of the IL2CPP metadata-usage table and MOVE on every
// game update. Re-derived for this build by disassembling the owning class's
// own methods and following  adrp/ldr -> R_AARCH64_RELATIVE addend -> slot,
// keeping only slots that are then dereferenced as  ldr x8,[klass,#0xB8]
// (Il2CppClass::static_fields) - i.e. exactly the access our reader performs.
inline constexpr std::uint64_t PLAYER_MANAGER_TYPEINFO_RVA       = 0xD9841E0;
inline constexpr std::uint64_t PLAYER_MANAGER_STATIC_FIELDS_LIST = 0x10; // clientPlayerList

inline constexpr std::uint64_t GAME_CONTROLLER_TYPEINFO_RVA         = 0xD97F598; // GameControllerBase
inline constexpr std::uint64_t GAME_CONTROLLER_LOCAL_PLAYER_FIELD   = 0x10; // <ukT>k__BackingField (PlayerManager)
inline constexpr std::uint64_t GAME_CONTROLLER_CAMERA_MANAGER_FIELD = 0x38; // <ukA>k__BackingField (CameraManager)
inline constexpr std::uint64_t CAMERA_MANAGER_CAMERA_FIELD          = 0x20; // m_Camera

// Oxide.PlayerManager instance fields (dump.cs)
inline constexpr std::uint64_t PLAYER_TRANSFORM = 0x68; // worldCameraRoot
// Prefer lastSavedPosition; lastTickPosition is adjacent at 0x1C8
inline constexpr std::uint64_t PLAYER_POSITION  = 0x1D4; // lastSavedPosition
inline constexpr std::uint64_t PLAYER_CHARACTER_MODEL = 0x150; // characterModel (UnityEngine.GameObject)
inline constexpr std::uint64_t PLAYER_NICKLABEL = 0x130; // nicklabel (class ij, was wK)
// PlayerManager "uWc" (0x220, was LLI): the real human-readable display name (confirmed
// on-device — equals the nicklabel widget's private name string, e.g.
// "пахановский" / "#Фришка"), whereas userID/voice names carry the machine id.
inline constexpr std::uint64_t PLAYER_DISPLAY_NAME = 0x220;

// nicklabel (class ij, was wK): player back-ref + nickname UI.Text.
inline constexpr std::uint64_t NICKLABEL_PLAYER_BACKREF = 0x20;
inline constexpr std::uint64_t NICKLABEL_NICKNAME_TEXT  = 0x38;
// UnityEngine.UI.Text: managed string with the visible nickname.
inline constexpr std::uint64_t UI_TEXT_MTEXT = 0xE0;
// FPObject (base of FPWeaponBase): display name of the held weapon.
inline constexpr std::uint64_t FPOBJECT_OBJECT_NAME = 0x78;
// Dissonance voice identity: reliable synced display-name source.
// PlayerManager.uWr (0x2E8, was LLT) : VoicePlayerState -> <Name>k__BackingField.
inline constexpr std::uint64_t PLAYER_VOICE_STATE  = 0x2F8;
inline constexpr std::uint64_t VOICE_STATE_NAME     = 0x38;
// PlayerManager.voicePlayer (class pJk, was fuI) -> display string at 0x78.
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
//   PlayerManager.playerEventHandler (0x78) -> class Gum (was DqO)
//   handler.manager (0xD0) back-ref == PlayerManager (validation)
//   handler.Aim (0x270, was 0x268) -> toggle activity (class Gub, was Dqg), its
//   <...>k__BackingField (0x10) == Active
//   This exact byte is what FPManager.LateUpdate reads to blend the camera to aimFOV.
//   The handler class is a live target: builds add/remove activities in the middle
//   of it. A KnockDoor field inserted at 0x188 shifted every later field by +8, so
//   Aim moved 0x268 -> 0x270 and 0x268 became Jump ("only in scope" silently read
//   the jump flag). offsets_map.json reaches these through `via`
//   (playerEventHandler -> Aim), so update_offsets.py verifies them by field name.
// Fallback: PlayerManager.fpManager (0x90) -> FPManager.LtZ (0x58, current FPWeaponBase)
//   FPObject.Player (0xC0) back-ref == PlayerManager (validation)
//   FPWeaponBase.<LKk>k__BackingField (0x120) == isAiming (set in weapon StartAim)
//   FPManager.<LtX>k__BackingField (0xA8) == aim blend 0..1 (secondary hint)
inline constexpr std::uint64_t PLAYER_EVENT_HANDLER          = 0x78;
inline constexpr std::uint64_t EVENT_HANDLER_MANAGER_BACKREF = 0xD0;
inline constexpr std::uint64_t EVENT_HANDLER_AIM_ACTIVITY    = 0x290;
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
inline constexpr std::uint64_t INV_PLAYER_INVENTORY_CLIENT = 0x28; // _playerInventoryClient

// ---- Remote (third-person) held weapon, from dump.cs ------------------------
// The FP objects above are local-only MonoBehaviours, so enemies never resolve
// through them. What every client *does* get is the networked weapon component:
//   HyperHug.Games.Oxide.Features.Weapons.PlayerWeapon : Mirror.NetworkBehaviour
//     playerWeaponViewReference  0xD0  (-> class sR, third-person weapon view)
//     Oxide.WeaponPiece          0x100 (SyncVar "weaponPiece", 0x10 bytes)
//     WeaponState                0x110
//     <player>k__BackingField    0x128 (back-ref to PlayerManager, validation)
// It hangs off PlayerManager.weaponReference (0xF0), which is an obfuscated
// lazy-reference wrapper just like kccReference, so it needs the same probing.
// This build inserted 0x40 bytes of new fields before `animator`, so every
// field from there on moved (old 0x90/0xD8/0xE8/0x100). The SyncVar trio was
// re-identified positionally against the _Mirror_SyncVarHookDelegate__loaded /
// _weaponPiece / _weaponState tail, not by name (names are re-obfuscated).
// NB: the new layout also has a second WeaponPiece at 0xA8 - that one is NOT
// the SyncVar and must not be used.
inline constexpr std::uint64_t PLAYERWEAPON_VIEW           = 0xE0;
inline constexpr std::uint64_t PLAYERWEAPON_PIECE          = 0x110;
inline constexpr std::uint64_t PLAYERWEAPON_STATE          = 0x120;
inline constexpr std::uint64_t PLAYERWEAPON_PLAYER_BACKREF = 0x138;
// Oxide.WeaponPiece (value type): Enabled 0x0, Number 0x2, Skin 0x4,
// SkinLevel 0x6, Loaded 0x7, Mods 0x8. Number is the item id of the weapon.
inline constexpr std::uint64_t WEAPONPIECE_ENABLED = 0x00;
inline constexpr std::uint64_t WEAPONPIECE_NUMBER  = 0x02;
// The third-person weapon view (class sR, was Mo; interface sY, was Ms).
// Found after the rename by field shape, not by name - see tools/offsets:
//   il2cpp_layout.py find "WeaponBase_o*" "Oxide_WeaponPiece_o" "UnityEngine_Transform_o*"
//   WeaponBase  0x48  -> MonoBehaviour on the spawned weapon GameObject
//   WeaponPiece 0x50
//   Transform   0x60  -> root transform of the spawned weapon
// A decorator (class pGW, was fSN) wraps another view at +0x10, so unwrap
// one level when needed. All four offsets survived the update unchanged.
inline constexpr std::uint64_t WEAPONVIEW_WEAPON_BASE    = 0x48;
inline constexpr std::uint64_t WEAPONVIEW_PIECE          = 0x50;
inline constexpr std::uint64_t WEAPONVIEW_ROOT_TRANSFORM = 0x60;
inline constexpr std::uint64_t WEAPONVIEW_INNER          = 0x10;
// HyperHug...Player.PlayerModelInfo — the character model rig. The equipment
// view parents the weapon prefab under these holders, so their first child is
// the weapon GameObject (name = prefab name).
//   head 0x20, rightWeaponHolder 0x28, leftWeaponHolder 0x30,
//   equipmentHolder 0x38, characterAnimation 0x60
inline constexpr std::uint64_t MODELINFO_RIGHT_WEAPON_HOLDER = 0x28;
inline constexpr std::uint64_t MODELINFO_LEFT_WEAPON_HOLDER  = 0x30;
// CharacterAnimation.playerModelInfo (0x30) and PlayerInventoryData
// .playerModelInfo (0x20) are the two ways to reach PlayerModelInfo.
inline constexpr std::uint64_t CHARANIM_PLAYER_MODEL_INFO = 0x30;
inline constexpr std::uint64_t INVDATA_PLAYER_MODEL_INFO  = 0x20;
// Il2CppClass: name at 0x10, namespace at 0x18 (used to identify PlayerWeapon).
inline constexpr std::uint64_t IL2CPP_CLASS_NAME      = 0x10;
inline constexpr std::uint64_t IL2CPP_CLASS_NAMESPACE = 0x18;
// eventHandler.LookDirection (0x140): synced Vector3 wrapper, value at +0x20.
// MouseLook.Update writes m_LookRoot.forward into it every frame and
// FPHitscan casts its hit ray along it (not along the camera transform).
inline constexpr std::uint64_t EVENT_HANDLER_LOOK_DIRECTION  = 0x140;
// Отметка выстрела: FPHitscan.ZDW(Ray) кладёт сюда точку попадания (0x170) и
// время (0x178) — по ним в логе видно, когда именно стреляли и что лежало в оси.
inline constexpr std::uint64_t EVENT_HANDLER_LAST_HIT_POINT  = 0x170;
inline constexpr std::uint64_t EVENT_HANDLER_LAST_HIT_TIME   = 0x178;
inline constexpr std::uint64_t SYNC_VALUE_OFFSET             = 0x20;
inline constexpr std::uint64_t ACTIVITY_ACTIVE_FLAG          = 0x10;
inline constexpr std::uint64_t PLAYER_FP_MANAGER             = 0x90;
inline constexpr std::uint64_t FPMANAGER_CURRENT_WEAPON      = 0x58;
inline constexpr std::uint64_t FPMANAGER_CURRENT_OBJECT      = 0x50; // _currentWeapon (FPObject fallback)
inline constexpr std::uint64_t FPMANAGER_AIM_BLEND           = 0xA8;
inline constexpr std::uint64_t FPOBJECT_PLAYER_BACKREF       = 0xC0;
inline constexpr std::uint64_t FPWEAPON_IS_AIMING            = 0x120;

// Максимальная дальность удара ближним орудием (топор/кирка/пила/копьё).
// Из дампа 62a8534, класс Oxide.FPMelee (наследники: FPTool -> FPChainsaw,
// FPSpear, FPBuildingHammer, FPTorch — имена читаемые, не ротируют):
//   FPMelee.m_MaxReach (0x128) + FPMelee.hitRadius (0x12C)
// Игра засчитывает удар в FPMelee.ZkX() ровно так:
//   GKo data = handler.RaycastData (0x160), если невалиден — handler.AimRaycast (0x168)
//   if (data.RaycastHit.distance < m_MaxReach + hitRadius) On_Hit(data); else On_Woosh();
// distance — это UnityEngine.RaycastHit.get_distance(), то есть 3D-метры ОТ
// КАМЕРЫ (луч строит GuL.ZJP из позы камеры/головы), а не по горизонтали.
// Значения сериализованы в префабе каждого инструмента; в конструкторе FPMelee
// стоят заглушки m_MaxReach=0.5, hitRadius=0.1 (m_TimeBetweenAttacks=0.85,
// m_DamagePerHit=15, m_ImpactForce=15), поэтому читать их надо из живого объекта.
// Лучи кастует Oxide.RaycastManager (FPObject.RaycastManager, 0x90):
//   m_RayLength (0x38, default 1.5) — длина обычного луча -> RaycastData;
//   m_AimRayLength (0x3C) и радиус сферы (0x40) — при доставании орудия
//   FPMelee.On_Draw кладёт туда свои m_MaxReach и hitRadius (RaycastManager.ZIj),
//   сфера -> AimRaycast; m_TooCloseThreeshold (0x44, default 1.0).
inline constexpr std::uint64_t FPOBJECT_RAYCAST_MANAGER      = 0x90;
inline constexpr std::uint64_t FPOBJECT_EVENT_HANDLER        = 0xC8; // Gum (PlayerEventHandler)
inline constexpr std::uint64_t FPMELEE_MAX_REACH             = 0x128;
inline constexpr std::uint64_t FPMELEE_HIT_RADIUS            = 0x12C;
// Ритм ударов самого орудия (секунды, сериализованы в префабе; в конструкторе
// FPMelee стоят 0.85 и 0.15). Тап быстрее m_TimeBetweenAttacks игра ставит в
// очередь и часть ударов съедает, медленнее — простой; поэтому такт бота берём
// отсюда, а не из подобранных на глаз миллисекунд.
inline constexpr std::uint64_t FPMELEE_TIME_BETWEEN_ATTACKS  = 0x130;
inline constexpr std::uint64_t FPMELEE_PAUSE_AFTER_ATTACK    = 0x134;
// Oxide.FPTool : Oxide.FPMelee — поля есть только у FPTool и FPChainsaw
// (прочие FPMelee: FPSpear, FPBuildingHammer, FPTorch — там на 0x160 своё).
inline constexpr std::uint64_t FPTOOL_TOOL_PURPOSES          = 0x160; // enum ToolPurpose, флаги
// Рядом (не читаем): 0x164 m_Efficiency (множитель урона), 0x168 alwaysCrit,
// 0x169 useAmmo, 0x16C attacksPerAmmo.
inline constexpr std::uint64_t RAYCASTMAN_PLAYER             = 0x20;
inline constexpr std::uint64_t RAYCASTMAN_RAY_LENGTH         = 0x38;
inline constexpr std::uint64_t RAYCASTMAN_AIM_RAY_LENGTH     = 0x3C;
inline constexpr std::uint64_t RAYCASTMAN_SPHERE_RADIUS      = 0x40;
inline constexpr std::uint64_t RAYCASTMAN_TOO_CLOSE          = 0x44;
// Что прямо сейчас видит прицел — результат лучей RaycastManager, разложенный
// в активности PlayerEventHandler (класс Gum). FPMelee.ZkX берёт их ровно так
// (дизассемблер 0x6533a20):
//   handler = weapon[0xC8]; data = handler[0x160][0x20]  (RaycastData)
//   if (data == null)       data = handler[0x168][0x20]  (AimRaycast)
//   if (data != null && data.RaycastHit.distance < m_MaxReach + hitRadius) Hit
// «Валидность» — это просто data != null (геттер 0x654fc58: cmp x0,#0; cset).
// Отсюда же берём «не перекрыт ли узел»: если луч игры упёрся ближе, чем наша
// точка прицела, удар уйдёт в перекрытие, а не в ресурс.
inline constexpr std::uint64_t GUM_RAYCAST_DATA              = 0x160; // GuI`1<GKo>
inline constexpr std::uint64_t GUM_AIM_RAYCAST               = 0x168; // GuI`1<GKo>
inline constexpr std::uint64_t GUI_VALUE                     = 0x20;  // значение внутри GuI`1<T>
inline constexpr std::uint64_t GKO_HIT_OBJECT                = 0x18;  // GameObject попадания
inline constexpr std::uint64_t GKO_RAYCAST_HIT               = 0x48;  // UnityEngine.RaycastHit (0x2C)
inline constexpr std::uint64_t RAYCASTHIT_DISTANCE           = 0x1C;  // m_Distance внутри RaycastHit
// Тот же struct UnityEngine.RaycastHit (dump.cs 62a8534, строка 642061):
//   m_Point 0x0 (Vector3), m_Normal 0xC (Vector3), m_FaceID 0x18,
//   m_Distance 0x1C, m_UV 0x20, m_Collider 0x28  — размер 0x2C.
inline constexpr std::uint64_t RAYCASTHIT_POINT              = 0x00;  // куда луч упёрся (мир)
inline constexpr std::uint64_t RAYCASTHIT_NORMAL             = 0x0C;  // нормаль поверхности там
// m_Collider — В КАКОМ коллайдере луч остановился. Нужен, чтобы отличить «луч
// упёрся в сам узел добычи» от «узел перекрыт чужой геометрией»: у камня точка
// прицела лежит внутри породы, поэтому луч всегда доходит до неё раньше и без
// этого поля собственный камень выглядел стеной (см. ray_hit_is_self_node).
inline constexpr std::uint64_t RAYCASTHIT_COLLIDER           = 0x28;  // UnityEngine.Collider

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
inline constexpr std::uint64_t KCC_MOVE                   = 0x154;
inline constexpr std::uint64_t KCC_CHARACTER_ANIMATION     = 0xF0;
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

// ---- World markers: ore nodes and animals (dump.cs + libil2cpp) -------------
// Both are Oxide.MineableObject subclasses (MineableStone / MineableAnimal /
// MineableTree / MineableObjectWithRandomSpawn), each a Mirror.NetworkBehaviour,
// so the client-side Mirror registry lists all of them:
//   Mirror.NetworkClient.spawned : Dictionary<uint, NetworkIdentity>
// The NetworkClient TypeInfo slot was taken from libil2cpp.so: the code does
//   adrp x19,#<page> ; ldr x19,[x19,#<off>]   (GOT entry)
// and the R_AARCH64_RELATIVE addend of that entry is the .data slot below.
// Verified by the static-field access pattern in NetworkClient's own methods:
//   ldr x0,[x19] ; ldr x8,[x0,#0xb8] (static_fields) ; ldr x0,[x8,#0x28] (spawned)
inline constexpr std::uint64_t NETWORK_CLIENT_TYPEINFO_RVA = 0xD983448;
inline constexpr std::uint64_t NETWORK_CLIENT_SPAWNED      = 0x28;

// System.Collections.Generic.Dictionary<uint, NetworkIdentity> (this BCL has no
// _fastModMultiplier): _buckets 0x10, _entries 0x18, _count 0x20, _freeList 0x24.
// Entry { int hashCode; int next; uint key; object value; } -> 24 bytes.
inline constexpr std::uint64_t DICT_ENTRIES      = 0x18;
inline constexpr std::uint64_t DICT_COUNT        = 0x20;
inline constexpr std::uint64_t DICT_ENTRY_STRIDE = 0x18;
inline constexpr std::uint64_t DICT_ENTRY_VALUE  = 0x10;

// Mirror.NetworkIdentity
inline constexpr std::uint64_t NETID_NET_ID     = 0x58; // uint netId
inline constexpr std::uint64_t NETID_BEHAVIOURS = 0x80; // NetworkBehaviour[]

// ---- Постройки: ХП дверей/стен (Oxide.PieceVitals : GenericVitals : pmp) ----
// pmp.Entity (0x68) -> pmK; pmK.Health (0x98) -> AsyncReactiveProperty<float>,
// текущее значение в latestValue (0x18). Максимум — GenericVitals.m_MaxHealth.
inline constexpr std::uint64_t PMP_ENTITY            = 0x68; // pmK Entity
inline constexpr std::uint64_t PMK_HEALTH            = 0x98; // AsyncReactiveProperty<float>
inline constexpr std::uint64_t ARP_LATEST_VALUE      = 0x18; // float latestValue
inline constexpr std::uint64_t VITALS_MAX_HEALTH     = 0x88; // GenericVitals.m_MaxHealth

// ---- Время суток: TOD_Sky (ассет Time Of Day) ------------------------------
// Oxide.TimeOfDay в боевых сценах не живёт: его ленивые метадата-слоты не
// инициализированы (Awake ни разу не вызывается), поэтому day/night читается
// из TOD_Sky. Имя его класса ОБФУСЦИРОВАНО И РОТИРУЕТ КАЖДЫЙ БИЛД: в дампе
// 89e0b63 это `IY`, в этом (62a8534) — `UV`. Структура же стабильна:
//   * единственный статик — List<Self> (первое статик-поле, offset 0x0);
//   * у инстанса `TOD_CycleParameters_o* Cycle` на 0x40;
//   * у TOD_CycleParameters: Hour(float)@0x10, Day@0x14, Month@0x18, Year@0x1C.
// Имя TOD_CycleParameters не обфусцировано — по нему класс и находится:
//   grep -n 'TOD_CycleParameters_o\* Cycle' il2cpp.h   # -> 0x40 = TOD_Sky
//
// TOD_SCAN_RVA_* — окно в .data.rel.ro, где лежат глобальные Il2CppClass*-слоты;
// day_tod_scan() в game.cpp перебирает его по 1 КБ за кадр и валидирует каждый
// кандидат по структуре выше. Это НЕ поля, но уезжают они КАЖДЫЙ билд вместе
// со всеми *_TYPEINFO_RVA — проверить:
//   python3 tools/offsets/typeinfo_rva.py --so <new>/libil2cpp.so
//       --script <new>/script.json --methods 2000 UV
// оба кандидата TOD_Sky (здесь 0xD8DF4C8 и 0xD8DFC98) обязаны попасть в окно.
// Прошлое окно было 0xD7A0000..0xD840000 — сдвинулось ровно на +0x130000.
inline constexpr std::uint64_t TOD_SCAN_RVA_BEGIN = 0xD970000;
inline constexpr std::uint64_t TOD_SCAN_RVA_END   = 0xDA10000; // BEGIN + 0xA0000
inline constexpr std::uint64_t TOD_SKY_CYCLE      = 0x40; // TOD_Sky.Cycle
inline constexpr std::uint64_t TOD_CYCLE_HOUR     = 0x10; // TOD_CycleParameters.Hour
inline constexpr std::uint64_t TOD_CYCLE_DAY      = 0x14;
inline constexpr std::uint64_t TOD_CYCLE_MONTH    = 0x18;
inline constexpr std::uint64_t TOD_CYCLE_YEAR     = 0x1C;

// Oxide.MineableObject — the shared base of ore nodes, trees and animals.
// Всё сверено с дампом 62a8534 (il2cpp.h: Oxide_MineableObject_Fields).
inline constexpr std::uint64_t MINEABLE_LOOT           = 0xA0; // List<Oxide.LootItem>
inline constexpr std::uint64_t MINEABLE_FINISH_BONUS   = 0xA8;
inline constexpr std::uint64_t MINEABLE_CURRENT_HEALTH = 0x78; // float, убывает от каждого удара
inline constexpr std::uint64_t MINEABLE_HIT_ANCHOR     = 0xC8; // LXX, Transform точки удара (см. ниже)
// Oxide.LootItem: the item short name the node drops ("stone", "metal.ore", ...)
inline constexpr std::uint64_t LOOTITEM_ITEM_NAME      = 0x10;
inline constexpr std::uint64_t MINEABLE_MAX_HEALTH     = 0xC0; // float
// LXX (0xC8) — Transform-якорь точки удара: ZgL() отдаёт его мировую позицию, а
// hitInfo (MineableObjectHitInfoCompact) несёт её дальше в экстеншены. У руды
// именно к этой точке игра привязывает Collider.ClosestPoint при первом ударе
// (OreHitstreaks.giu), у дерева — рейкаст по стволу. Нужен как запасная точка
// прицела на руде: пивот камня стоит ВНУТРИ породы, и прицел по нему оставляет
// луч игры на поверхности раньше точки прицела. Читается только в диагностике
// и только когда цель — руда.
inline constexpr std::uint64_t MINEABLE_FRACTION       = 0xD0; // fractionRemaining
inline constexpr std::uint64_t MINEABLE_ENTITY_TYPE    = 0xD8; // ServerPlayersAnalytics.EntityType
// Каким орудием узел вообще добывается. Тип — тот же enum Oxide.FPTool.
// ToolPurpose, что и у орудия в руке (FPTool.m_ToolPurposes), то есть сравнение
// побитовое: (purposes & required) != 0. Кирка не рубит дерево и наоборот —
// без этой проверки бот вечно кружил вокруг узла, который нечем взять.
inline constexpr std::uint64_t MINEABLE_REQUIRED_TOOL_PURPOSE = 0x70; // int32, флаги
// Сколько опыта даёт узел (int32). Только для строки статуса.
inline constexpr std::uint64_t MINEABLE_EXPERIENCE     = 0xD4;

enum class ToolPurpose : std::int32_t {
    None       = 0,
    CutWood    = 1, // топор/пила
    BreakRocks = 2, // кирка
    CutAnimals = 4, // нож
};
// Кеш JE-экстеншенов узла (MineableObjectExtension_*). Заполняется в
// MineableObject.cik() через GetComponents<JE> на GameObject узла; cik
// вызывается из OnStartClient -> ciQ() (то есть кеш готов сразу при спавне
// узла на клиенте) и при каждом попадании (SRl -> cik). Именно здесь живёт
// «крестик» ресурса — см. блок ниже.
inline constexpr std::uint64_t MINEABLE_EXTENSIONS     = 0xE8; // JE[]

// ---- Крестик (hit-streak marker) --------------------------------------------
// «Крестик» — не часть меша дерева/камня, а отдельный объект, который игра
// создаёт при первом же попадании по узлу:
//
//   MineableObject.SRl(локальный удар) -> cik() -> JE.gir(hitInfo)
//     руда:   OreHitstreaks.gir -> giu() (Collider.ClosestPoint — точка на
//             поверхности камня) -> gil() (Instantiate + SetParent +
//             set_position) -> MoW = живой клон; его transform и есть X.
//     дерево: TreeHitstreaks.gir -> raycast по стволу -> Instantiate ->
//             MTn = живой клон, а мировая точка попадания пишется в MTQ/MTu.
//
// Проверка «попал ли удар в крестик» (OreHitstreaks.os для руды и
// TreeHitstreaks.giL + вся его обвязка rRS/DMU/Dne/DfW для деревьев) меряет
// дистанцию от точки удара ИМЕННО до этих полей: для руды — до позиции
// transform'а маркера, для дерева — до отрезка MTQ..MTu (радиус 0.15 м).
// Поэтому они и есть единственно верная цель автофарма: удар по ним игра
// засчитывает как попадание в серию, а всё остальное (корпус, LOD-меш,
// спящий шаблон на пивоте) — нет.

// MineableObjectExtension_OreHitstreaks : JE — руда/камень/сера.
inline constexpr std::uint64_t OREHS_STREAK_INDEX      = 0x20; // int hitstreakIndex
inline constexpr std::uint64_t OREHS_MARKER_TEMPLATE   = 0x28; // спящий шаблон на пивоте узла
inline constexpr std::uint64_t OREHS_MARKER            = 0x30; // MoW — живой клон (X), null до первого удара
inline constexpr std::uint64_t OREHS_COLLIDER          = 0x38;
inline constexpr std::uint64_t OREHS_MINEABLE          = 0x40; // обратная ссылка на узел

// MineableObjectExtension_OreHitstreaksMarker : MonoBehaviour — сам X.
// Мировая позиция его transform'а = точка крестика. lHG (0x58) — таймер
// жизни: Update() (RVA 0x786eb40) делает `lHG += Time.deltaTime` и сравнивает
// с 15.0 (`fmov s1,#15.0; fcmp s0,s1; b.le`); при превышении — SetActive(false)
// на своём GameObject и хвостовой вызов владельца (lzF, 0x38). Так что
// «поле OREHS_MARKER заполнено» != «крестик жив»: проверять надо возраст.
// Имена полей — из il2cpp.h дампа 62a8534.
// Полный расклад класса (для сверки при апдейте): 0x20 meshRenderer,
// 0x28 ignoringLayerMask, 0x30 sizeByDistance (AnimationCurve), 0x38 lzF
// (владелец), 0x40 lzN (MaterialPropertyBlock), 0x48 lzA, 0x50 lHh, 0x58 lHG.
inline constexpr std::uint64_t OREMARK_RENDERER        = 0x20; // MeshRenderer
inline constexpr std::uint64_t OREMARK_OWNER           = 0x38; // lzF — обратная ссылка на OreHitstreaks
inline constexpr std::uint64_t OREMARK_SMOOTH          = 0x48; // lzA, float — плавное значение размера
inline constexpr std::uint64_t OREMARK_AGE             = 0x58; // lHG, float — секунды с появления
inline constexpr float         OREMARK_LIFETIME        = 15.0F;

// MineableObjectExtension_TreeHitstreaks : JE — деревья.
inline constexpr std::uint64_t TREEHS_MOVING_METHOD    = 0x20; // enum MovingMethod (Static/AroundTree)
inline constexpr std::uint64_t TREEHS_MARKER_TEMPLATE  = 0x28; // HitMarkerItem — шаблон
inline constexpr std::uint64_t TREEHS_STREAK           = 0x48; // MTz (сбрасывается в gie)
inline constexpr std::uint64_t TREEHS_MARKER           = 0x50; // MTn — живой клон (X), null до первого удара
inline constexpr std::uint64_t TREEHS_SPOT_A           = 0x88; // MTQ, Vector3 — точка X в мировых
inline constexpr std::uint64_t TREEHS_SPOT_B           = 0xA4; // MTu, Vector3 — второй конец отрезка X

// MineableObjectExtension_HitMarkerItem : MonoBehaviour — визуальный X на
// дереве. mark (0x38) — managed Transform декаля: giI ставит его в точку
// попадания + normal*0.25 (чтобы не z-файтил с корой), поэтому для
// прицеливания первична MTQ (точка на коре), а декаль — запасной вариант.
// lifetime (0x20) — ПОЛНЫЙ срок жизни, а возраст маркер набирает сам в lzr
// (0xC8): Update() (RVA 0x786485c) делает `lzr += Time.deltaTime` и при
// `lzr > lifetime` отдаёт объект владельцу (lzT, 0xC0) — то есть в пул, где его
// переиспользуют для другого дерева. Живой X — только пока lzr <= lifetime.
inline constexpr std::uint64_t HITMARK_LIFETIME        = 0x20; // float, полный срок
inline constexpr std::uint64_t HITMARK_FILTER          = 0x28; // MeshFilter
inline constexpr std::uint64_t HITMARK_RENDERER        = 0x30; // Renderer
inline constexpr std::uint64_t HITMARK_MARK            = 0x38; // Transform самого крестика
inline constexpr std::uint64_t HITMARK_AGE             = 0xC8; // lzr, float — сколько уже прожито

// ServerPlayersAnalytics.EntityType values used for the labels.
enum class MineableEntityType : std::int32_t {
    None = 0, Bear = 1, Boar = 2, Deer = 3, Rabbit = 4, Chicken = 5, Fish = 6,
    Cannibal = 7, Tree = 8, Stone = 9, Iron = 10, Sulfur = 11, Ice = 12,
    Barrel = 13, Lootbox = 14, RoadSign = 15, StackOfWood = 16, Construction = 17,
    Deployable = 18, Human = 19, Player = 20, Vehicle = 21, Hare = 22,
    LootboxBaloon = 23, LootboxBaloonBig = 24, // added by this game update
};

// ---- Team / clan membership (Oxide.PlayerManager, dump.cs) ------------------
// All three are Mirror SyncVars (they have _Mirror_SyncVarHookDelegate_* twins
// at 0x378/0x390/0x398), so every client sees them for every player.
inline constexpr std::uint64_t PLAYER_USER_ID   = 0x278; // string userID (unique per account)
inline constexpr std::uint64_t PLAYER_TEAM_NAME = 0x280; // string teamName
inline constexpr std::uint64_t PLAYER_CLAN_ID   = 0x290; // string clanId
inline constexpr std::uint64_t PLAYER_CLAN_TAG  = 0x298; // string clanTag

// Vehicles: both are SyncVars (uint netId of the vehicle / index of the seat),
// zero while the player is on foot. A mounted player stops updating
// lastSavedPosition, so his box has to come from the rendered transform.
inline constexpr std::uint64_t PLAYER_VEHICLE_ID = 0x288; // uint vehicleID
inline constexpr std::uint64_t PLAYER_SEAT_ID    = 0x28C; // uint seatID

// ---- World loot containers (Oxide.LootObject : fNZ : Mirror.NetworkBehaviour)
// Everything openable in the world is a LootObject: road crates, barrels,
// airdrops — and the storage boxes players deploy. The deployed ones are
// building pieces, so m_Piece is the discriminator that keeps them off screen.
inline constexpr std::uint64_t LOOTOBJECT_INVENTORY      = 0xA0; // Oxide.Inventory
inline constexpr std::uint64_t LOOTOBJECT_IS_LOOTABLE    = 0xA8; // bool
inline constexpr std::uint64_t LOOTOBJECT_PANEL_NAME     = 0xE0; // string panelName
// 0xF0 in the previous build: a new  System.String m_ContainerSoundKey  field
// was inserted at 0xE8, pushing m_Piece and everything after it up by 8.
// (panelName @0xE0 sits before the insert and is unchanged.)
inline constexpr std::uint64_t LOOTOBJECT_BUILDING_PIECE = 0xF8; // Building.BuildingPiece m_Piece

// ---- Ground pickups (Oxide.ItemPickup : fNZ : Mirror.NetworkBehaviour) ------
// Everything lying on the ground that can be picked up: mushrooms, berries,
// dropped items, harvested resources. It carries the item short name and the
// stack size directly, so no inventory walk is needed.
inline constexpr std::uint64_t ITEMPICKUP_ITEM_OBJECT = 0xA8; // Oxide.Item
inline constexpr std::uint64_t ITEMPICKUP_SHORTNAME   = 0xD8; // string item
inline constexpr std::uint64_t ITEMPICKUP_AMOUNT      = 0xE0; // int amount

// Oxide.GameControllerBase static fields: a known-good NetworkIdentity used to
// learn the NetworkIdentity class pointer (validates dictionary entries).
inline constexpr std::uint64_t GAME_CONTROLLER_NET_IDENTITY_FIELD = 0x8;

}

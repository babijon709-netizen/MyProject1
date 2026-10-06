#pragma once
#include <cstdint>

// Оффсеты beta-клиента (dump11943). Метод получения — такой же, как у
// релиза (см. game_offsets.h): il2cpp_dump.py от metadata v39 (XOR
// 0xa5c3f19d + перестановка троек), сопоставление с dump11945 по типам,
// порядку и соседям полей; TypeInfo — adrp/ldr -> outer GOT ->
// R_AARCH64_RELATIVE addend -> inner slot -> ldr [klass,#0xB8], плюс
// .cctor TOD-класса и сигнатуры чтений static fields.
//
// Результат сверки: managed-layouts двух билдов совпадают БАЙТ В БАЙТ во
// всех используемых классах, кроме одного поля (см. INVDATA_... ниже).
// Отличия только в TypeInfo inner-slots и окне TOD. Финансовые enum'ы
// (ToolPurpose, ServerPlayersAnalytics.EntityType) идентичны — общие enum
// живут в game_offsets.h и не дублируются здесь.
//
// Обфусцированные имена этого дампа (для следующей сверки):
//   PlayerEventHandler = maY (rel pLh); nicklabel = dL (rel Xq);
//   voicePlayer = mgn (rel prF); TOD_Sky = YQ (rel ZD), YQ::.cctor@0x682c2b8;
//   activity-классы pLv/pLx -> maU/mar; view-цепочка lL/lQ -> eR/eI.
//
// Native libunity.so беты (25030624 Б, отдельный архив): тот же Unity
// 6000.3.18f1_5ebeb53e4c07 (.note.unity идентична релизной побайтово).
// Функции с любыми native-оффсетами — побайтово одинаковые с релизом
// (различия только в abсолютных target'ах bl/b/adrp и сдвиге секций);
// сдвиги регионов вплоть до +0x134, глобально ~69% .text совпадает
// окнами по 32 байта при согласованных сдвигах. Контрольные точки
// сверены в ОБОИХ билдах: cam+0x70 (stp q0,q1 / copy-fn), cam+0xB0,
// dirty @0x500/@0x502 (strh/strb), aspect @0x4E0, fov @0x170,
// near/far @0x454/@0x458, childCount @0x58 (icall-заглушка),
// hierarchy matrices @0x18 / indices @0x20 (stride 0x30, index 4 байта).
// Не верифицированы инструкциями, но лежат в том же изоморфном коде:
// cam+0xF0 (worldToClip), cam+0x5C8 (prevViewProj), cam+0x20,
// transform children @0x48, hierarchy data/index @0x38/@0x40,
// component/go @0x20, pair @0x08, go name @0x48.

namespace game_offsets_beta {

// Native Camera: оффсеты инструкционно подтверждены в beta libunity.so
// (см. комментарий выше; подробности в reversing/OFFSETS_DUMP11943BETA_2026-10-06.md).
inline constexpr std::uint64_t CAMERA_PROJECTION_MATRIX = 0xB0;
inline constexpr std::uint64_t CAMERA_VIEW_MATRIX = 0x70;
inline constexpr std::uint64_t CAMERA_WORLD_TO_CLIP = 0xF0;
inline constexpr std::uint64_t CAMERA_PREV_VIEW_PROJ = 0x5C8;
inline constexpr std::uint64_t CAMERA_NATIVE_TRANSFORM = 0x20;
inline constexpr std::uint64_t CAMERA_FOV_DEGREES = 0x170;
inline constexpr std::uint64_t CAMERA_ASPECT = 0x4E0;
inline constexpr std::uint64_t CAMERA_NEAR_CLIP = 0x454;
inline constexpr std::uint64_t CAMERA_FAR_CLIP = 0x458;
inline constexpr std::uint64_t CAMERA_VIEW_DIRTY = 0x502;
inline constexpr std::uint64_t CAMERA_PROJ_DIRTY = 0x500;
inline constexpr std::uint64_t MANAGED_CACHED_PTR = 0x10;
inline constexpr std::uint64_t NETWORK_BEHAVIOUR_IDENTITY = 0x40;
inline constexpr std::uint64_t NETWORK_IDENTITY_IS_LOCAL_PLAYER = 0x4A;
inline constexpr std::uint64_t PLAYER_MANAGER_TYPEINFO_RVA = 0xDD732B8;  // beta: отличается от релиза (0xDCC45F8)
inline constexpr std::uint64_t PLAYER_LOOP_HELPER_TYPEINFO_RVA = 0xDD73280;  // beta: отличается от релиза (0xDCC45C8)
inline constexpr std::uint64_t PLAYER_MANAGER_STATIC_FIELDS_LIST = 0x10;
inline constexpr std::uint64_t IL2CPP_CLASS_STATIC_FIELDS_OFFSET = 0xB8;
inline constexpr std::uint64_t PLAYERLOOPHELPER_RUNNERS = 0x30;
inline constexpr std::uint64_t LOOPRUNNER_ITEMS = 0x38;
inline constexpr std::uint64_t LOOPRUNNER_TAIL = 0x30;
inline constexpr std::uint64_t DELAYPROMISE_TIMESPAN = 0x18;
inline constexpr std::uint64_t DELAYPROMISE_ELAPSED = 0x1C;
inline constexpr std::uint64_t DELAYPROMISE_INITIAL_FRAME = 0x20;
inline constexpr std::uint64_t DELAYPROMISE_SCALED_TIMESPAN = 0x1C;
inline constexpr std::uint64_t DELAYPROMISE_SCALED_ELAPSED = 0x20;
inline constexpr std::uint64_t DELAYPROMISE_SCALED_INITIAL_FRAME = 0x18;
inline constexpr std::uint64_t FPHITSCAN_CONFIG = 0x130;
inline constexpr std::uint64_t FPWEAPON_EFFECTIVE_RELOAD_PERIOD = 0xE8;
inline constexpr std::uint64_t FPWEAPON_SHOT_PERIOD = 0x124;
inline constexpr std::uint64_t FPHITSCAN_BURST_WAIT = 0x1F0;
inline constexpr std::uint64_t WAITFORSECONDS_SECONDS = 0x10;
inline constexpr std::uint64_t FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD = 0x2A0;
inline constexpr std::uint64_t FPHITSCAN_CALCULATOR = 0x1F8;
inline constexpr std::uint64_t CALCULATOR_DISPERSION_CONFIG = 0x10;
inline constexpr std::uint64_t CALCULATOR_HITSCAN_CONFIG = 0x18;
inline constexpr std::uint64_t CALCULATOR_CACHED_BASE_DISPERSION = 0x20;
inline constexpr std::uint64_t HITSCANCONFIG_FIRE_RATE = 0x34;
inline constexpr std::uint64_t HITSCANCONFIG_FULL_RELOAD = 0x48;
inline constexpr std::uint64_t HITSCANCONFIG_LOOP_RELOAD = 0x4C;
inline constexpr std::uint64_t HITSCANCONFIG_DISPERSION_CONFIG = 0x68;
inline constexpr std::uint64_t HITSCANCONFIG_RECOIL_CONFIG = 0x70;
inline constexpr std::uint64_t FPHITSCAN_RECOIL = 0x200;
inline constexpr std::uint64_t WEAPONRECOIL_RECOIL_CONFIG = 0x00;
inline constexpr std::uint64_t WEAPONRECOIL_WEAPON_CONFIG = 0x08;
inline constexpr std::uint64_t RECOILCONFIG_MIN_RECOIL = 0x18;
inline constexpr std::uint64_t DISPERSIONCONFIG_MIN_DISPERSION = 0x20;
inline constexpr std::uint64_t DISPERSIONCONFIG_MAX_DISPERSION = 0x24;
inline constexpr std::uint64_t GAME_CONTROLLER_TYPEINFO_RVA = 0xDD6E5F8;  // beta: отличается от релиза (0xDCBF840)
inline constexpr std::uint64_t GAME_CONTROLLER_LOCAL_PLAYER_FIELD = 0x10;
inline constexpr std::uint64_t GAME_CONTROLLER_CAMERA_MANAGER_FIELD = 0x38;
inline constexpr std::uint64_t CAMERA_MANAGER_CAMERA_FIELD = 0x20;
inline constexpr std::uint64_t PLAYER_TRANSFORM = 0x68;
inline constexpr std::uint64_t PLAYER_POSITION = 0x1F4;
inline constexpr std::uint64_t PLAYER_CHARACTER_MODEL = 0x150;
inline constexpr std::uint64_t PLAYER_NICKLABEL = 0x130;
inline constexpr std::uint64_t PLAYER_DISPLAY_NAME = 0x240;
inline constexpr std::uint64_t NICKLABEL_PLAYER_BACKREF = 0x20;
inline constexpr std::uint64_t NICKLABEL_NICKNAME_TEXT = 0x38;
inline constexpr std::uint64_t UI_TEXT_MTEXT = 0xE0;
inline constexpr std::uint64_t FPOBJECT_OBJECT_NAME = 0x78;
inline constexpr std::uint64_t PLAYER_VOICE_STATE = 0x328;
inline constexpr std::uint64_t VOICE_STATE_NAME = 0x38;
inline constexpr std::uint64_t PLAYER_VOICE_PLAYER = 0x140;
inline constexpr std::uint64_t VOICE_PLAYER_TAG = 0x78;
inline constexpr std::uint64_t FPOBJECT_ITEM = 0x40;
inline constexpr std::uint64_t ITEM_DATA = 0x20;
inline constexpr std::uint64_t ITEMDATA_NAME = 0x18;
inline constexpr std::uint64_t ITEMDATA_SHORTNAME = 0x20;
inline constexpr std::uint64_t IL2CPP_STRING_LENGTH = 0x10;
inline constexpr std::uint64_t IL2CPP_STRING_CHARS = 0x14;
inline constexpr std::uint64_t PLAYER_EVENT_HANDLER = 0x78;
inline constexpr std::uint64_t EVENT_HANDLER_MANAGER_BACKREF = 0xD0;
inline constexpr std::uint64_t EVENT_HANDLER_AIM_ACTIVITY = 0x298;
inline constexpr std::uint64_t PLAYER_MANAGER_RESPAWNING = 0x228;
inline constexpr std::uint64_t PLAYER_DEATH_HANDLER = 0x80;
inline constexpr std::uint64_t DEATH_HANDLER_REF_TARGET = 0x18;
inline constexpr std::uint64_t DEATH_HANDLER_DEATH = 0xF0;
inline constexpr std::uint64_t PLAYER_VITALS = 0xC8;
inline constexpr std::uint64_t VITALS_HEALTH = 0xB8;
inline constexpr std::uint64_t PLAYER_INVENTORY = 0x98;
inline constexpr std::uint64_t PLAYER_WEAPON_REFERENCE = 0xF0;
inline constexpr std::uint64_t PLAYER_WEAPONS_ARRAY = 0x198;
inline constexpr std::uint64_t INV_PLAYER_INVENTORY_DATA = 0x20;
inline constexpr std::uint64_t INV_PLAYER_INVENTORY_CLIENT = 0x28;
inline constexpr std::uint64_t PLAYERWEAPON_VIEW = 0xE8;
inline constexpr std::uint64_t PLAYERWEAPON_PIECE = 0x110;
inline constexpr std::uint64_t PLAYERWEAPON_STATE = 0x120;
inline constexpr std::uint64_t PLAYERWEAPON_PLAYER_BACKREF = 0x138;
inline constexpr std::uint64_t WEAPONPIECE_ENABLED = 0x00;
inline constexpr std::uint64_t WEAPONPIECE_NUMBER = 0x02;
inline constexpr std::uint64_t WEAPONVIEW_WEAPON_BASE = 0x48;
inline constexpr std::uint64_t WEAPONVIEW_PIECE = 0x50;
inline constexpr std::uint64_t WEAPONVIEW_ROOT_TRANSFORM = 0x60;
inline constexpr std::uint64_t WEAPONVIEW_INNER = 0x10;
inline constexpr std::uint64_t MODELINFO_RIGHT_WEAPON_HOLDER = 0x28;
inline constexpr std::uint64_t MODELINFO_LEFT_WEAPON_HOLDER = 0x30;
inline constexpr std::uint64_t CHARANIM_PLAYER_MODEL_INFO = 0x30;
inline constexpr std::uint64_t INVDATA_PLAYER_MODEL_INFO = 0x18;  // beta: отличается от релиза (0x20)
inline constexpr std::uint64_t IL2CPP_CLASS_NAME = 0x10;
inline constexpr std::uint64_t IL2CPP_CLASS_NAMESPACE = 0x18;
inline constexpr std::uint64_t EVENT_HANDLER_LOOK_DIRECTION = 0x140;
inline constexpr std::uint64_t EVENT_HANDLER_LAST_HIT_POINT = 0x170;
inline constexpr std::uint64_t EVENT_HANDLER_LAST_HIT_TIME = 0x178;
inline constexpr std::uint64_t SYNC_VALUE_OFFSET = 0x20;
inline constexpr std::uint64_t ACTIVITY_ACTIVE_FLAG = 0x10;
inline constexpr std::uint64_t PLAYER_FP_MANAGER = 0x90;
inline constexpr std::uint64_t FPMANAGER_CURRENT_WEAPON = 0x58;
inline constexpr std::uint64_t FPMANAGER_CURRENT_OBJECT = 0x50;
inline constexpr std::uint64_t FPMANAGER_AIM_BLEND = 0xA8;
inline constexpr std::uint64_t FPOBJECT_PLAYER_BACKREF = 0xC0;
inline constexpr std::uint64_t FPWEAPON_IS_AIMING = 0x120;
inline constexpr std::uint64_t FPOBJECT_RAYCAST_MANAGER = 0x90;
inline constexpr std::uint64_t FPOBJECT_EVENT_HANDLER = 0xC8;
inline constexpr std::uint64_t FPMELEE_MAX_REACH = 0x128;
inline constexpr std::uint64_t FPMELEE_HIT_RADIUS = 0x12C;
inline constexpr std::uint64_t FPMELEE_TIME_BETWEEN_ATTACKS = 0x130;
inline constexpr std::uint64_t FPMELEE_PAUSE_AFTER_ATTACK = 0x134;
inline constexpr std::uint64_t FPTOOL_TOOL_PURPOSES = 0x160;
inline constexpr std::uint64_t RAYCASTMAN_PLAYER = 0x20;
inline constexpr std::uint64_t RAYCASTMAN_RAY_LENGTH = 0x38;
inline constexpr std::uint64_t RAYCASTMAN_AIM_RAY_LENGTH = 0x3C;
inline constexpr std::uint64_t RAYCASTMAN_SPHERE_RADIUS = 0x40;
inline constexpr std::uint64_t RAYCASTMAN_TOO_CLOSE = 0x44;
inline constexpr std::uint64_t GUM_RAYCAST_DATA = 0x160;
inline constexpr std::uint64_t GUM_AIM_RAYCAST = 0x168;
inline constexpr std::uint64_t GUI_VALUE = 0x20;
inline constexpr std::uint64_t GKO_HIT_OBJECT = 0x18;
inline constexpr std::uint64_t GKO_RAYCAST_HIT = 0x48;
inline constexpr std::uint64_t RAYCASTHIT_DISTANCE = 0x1C;
inline constexpr std::uint64_t RAYCASTHIT_POINT = 0x00;
inline constexpr std::uint64_t RAYCASTHIT_NORMAL = 0x0C;
inline constexpr std::uint64_t RAYCASTHIT_COLLIDER = 0x28;
inline constexpr std::uint64_t PLAYER_KCC_REFERENCE = 0xB0;
inline constexpr std::uint64_t KCC_PLAYER_BACKREF = 0x78;
inline constexpr std::uint64_t KCC_HEAD_TRANSFORM = 0x88;
inline constexpr std::uint64_t KCC_NORMAL_HEIGHT = 0xA0;
inline constexpr std::uint64_t KCC_CROUCH_HEIGHT = 0xA4;
inline constexpr std::uint64_t KCC_HITBOX_ROOT = 0x70;
inline constexpr std::uint64_t HITBOX_ROOT_ARRAY = 0x68;
inline constexpr std::uint64_t HITBOX_SIZE = 0x24;
inline constexpr std::uint64_t HITBOX_CENTER = 0x30;
inline constexpr std::uint64_t HITBOX_AREA = 0x68;
inline constexpr std::uint64_t KCC_LOOK_HEIGHT_OFFSET = 0x90;
inline constexpr std::uint64_t KCC_MOVE = 0x13C;
inline constexpr std::uint64_t KCC_CHARACTER_ANIMATION = 0xE0;
inline constexpr std::uint64_t CHAR_ANIM_PLAYER_BACKREF = 0x78;
inline constexpr std::uint64_t CHAR_ANIM_RAGDOLL = 0x38;
inline constexpr std::uint64_t RAGDOLL_PELVIS_RIGIDBODY = 0x20;
inline constexpr std::uint64_t RAGDOLL_BONES_ARRAY = 0x88;
inline constexpr std::uint64_t RAGDOLL_BODYPART_TRANSFORM = 0x10;
inline constexpr std::uint64_t IL2CPP_ARRAY_LENGTH = 0x18;
inline constexpr std::uint64_t TRANSFORM_CHILDREN_ARRAY = 0x48;
inline constexpr std::uint64_t TRANSFORM_CHILD_COUNT = 0x58;
inline constexpr std::uint64_t TRANSFORM_HIERARCHY_DATA = 0x38;
inline constexpr std::uint64_t TRANSFORM_HIERARCHY_INDEX = 0x40;
inline constexpr std::uint64_t TRANSFORM_HIERARCHY_MATRICES = 0x18;
inline constexpr std::uint64_t TRANSFORM_HIERARCHY_INDICES = 0x20;
inline constexpr std::uint64_t COMPONENT_GAMEOBJECT = 0x20;
inline constexpr std::uint64_t GAMEOBJECT_COMPONENT_ARRAY = 0x20;
inline constexpr std::uint64_t COMPONENT_PAIR_PTR = 0x08;
inline constexpr std::uint64_t GAMEOBJECT_NAME = 0x48;
inline constexpr std::uint64_t GAMEOBJECT_NAME_GUESS = GAMEOBJECT_NAME;
inline constexpr std::uint64_t IL2CPP_LIST_ITEMS = 0x10;
inline constexpr std::uint64_t IL2CPP_LIST_SIZE = 0x18;
inline constexpr std::uint64_t IL2CPP_ARRAY_FIRST_ELEMENT = 0x20;
inline constexpr std::uint64_t NETWORK_CLIENT_TYPEINFO_RVA = 0xDD725C8;  // beta: отличается от релиза (0xDCC3860)
inline constexpr std::uint64_t NETWORK_CLIENT_SPAWNED = 0x30;
inline constexpr std::uint64_t DICT_ENTRIES = 0x18;
inline constexpr std::uint64_t DICT_COUNT = 0x20;
inline constexpr std::uint64_t DICT_ENTRY_STRIDE = 0x18;
inline constexpr std::uint64_t DICT_ENTRY_VALUE = 0x10;
inline constexpr std::uint64_t NETID_NET_ID = 0x58;
inline constexpr std::uint64_t NETID_BEHAVIOURS = 0x80;
inline constexpr std::uint64_t PMP_ENTITY = 0x68;
inline constexpr std::uint64_t PMK_HEALTH = 0x98;
inline constexpr std::uint64_t ARP_LATEST_VALUE = 0x18;
inline constexpr std::uint64_t HEALTH_VALUE = 0x20;
inline constexpr std::uint64_t VITALS_MAX_HEALTH = 0x88;
inline constexpr std::uint64_t TOD_SCAN_RVA_BEGIN = 0xDD70000;  // beta: отличается от релиза (0xDCC0000)
inline constexpr std::uint64_t TOD_SCAN_RVA_END = 0xDE30000;  // beta: отличается от релиза (0xDD80000)
inline constexpr std::uint64_t TOD_SKY_CYCLE = 0x40;
inline constexpr std::uint64_t TOD_CYCLE_HOUR = 0x10;
inline constexpr std::uint64_t TOD_CYCLE_DAY = 0x14;
inline constexpr std::uint64_t TOD_CYCLE_MONTH = 0x18;
inline constexpr std::uint64_t TOD_CYCLE_YEAR = 0x1C;
inline constexpr std::uint64_t MINEABLE_LOOT = 0xA0;
inline constexpr std::uint64_t MINEABLE_FINISH_BONUS = 0xA8;
inline constexpr std::uint64_t MINEABLE_CURRENT_HEALTH = 0x78;
inline constexpr std::uint64_t MINEABLE_HIT_ANCHOR = 0xC8;
inline constexpr std::uint64_t LOOTITEM_ITEM_NAME = 0x10;
inline constexpr std::uint64_t MINEABLE_MAX_HEALTH = 0xC0;
inline constexpr std::uint64_t MINEABLE_FRACTION = 0xD0;
inline constexpr std::uint64_t MINEABLE_ENTITY_TYPE = 0xD8;
inline constexpr std::uint64_t MINEABLE_REQUIRED_TOOL_PURPOSE = 0x70;
inline constexpr std::uint64_t MINEABLE_EXPERIENCE = 0xD4;
inline constexpr std::uint64_t MINEABLE_EXTENSIONS = 0xE8;
inline constexpr std::uint64_t OREHS_STREAK_INDEX = 0x20;
inline constexpr std::uint64_t OREHS_MARKER_TEMPLATE = 0x28;
inline constexpr std::uint64_t OREHS_MARKER = 0x30;
inline constexpr std::uint64_t OREHS_COLLIDER = 0x38;
inline constexpr std::uint64_t OREHS_MINEABLE = 0x40;
inline constexpr std::uint64_t OREMARK_RENDERER = 0x20;
inline constexpr std::uint64_t OREMARK_OWNER = 0x38;
inline constexpr std::uint64_t OREMARK_SMOOTH = 0x48;
inline constexpr std::uint64_t OREMARK_AGE = 0x58;
inline constexpr std::uint64_t TREEHS_MARKER_TEMPLATE = 0x28;
inline constexpr std::uint64_t TREEHS_STREAK = 0x48;
inline constexpr std::uint64_t TREEHS_MARKER = 0x50;
inline constexpr std::uint64_t TREEHS_SPOT_A = 0x88;
inline constexpr std::uint64_t TREEHS_SPOT_B = 0xA4;
inline constexpr std::uint64_t HITMARK_LIFETIME = 0x20;
inline constexpr std::uint64_t HITMARK_FILTER = 0x28;
inline constexpr std::uint64_t HITMARK_RENDERER = 0x30;
inline constexpr std::uint64_t HITMARK_MARK = 0x38;
inline constexpr std::uint64_t HITMARK_AGE = 0xC8;
inline constexpr std::uint64_t PLAYER_USER_ID = 0x298;
inline constexpr std::uint64_t PLAYER_TEAM_NAME = 0x2A0;
inline constexpr std::uint64_t PLAYER_CLAN_ID = 0x2B0;
inline constexpr std::uint64_t PLAYER_CLAN_TAG = 0x2B8;
inline constexpr std::uint64_t PLAYER_VEHICLE_ID = 0x2A8;
inline constexpr std::uint64_t PLAYER_SEAT_ID = 0x2AC;
inline constexpr std::uint64_t LOOTOBJECT_INVENTORY = 0xA0;
inline constexpr std::uint64_t LOOTOBJECT_IS_LOOTABLE = 0xA8;
inline constexpr std::uint64_t LOOTOBJECT_PANEL_NAME = 0xE0;
inline constexpr std::uint64_t LOOTOBJECT_BUILDING_PIECE = 0xF8;
inline constexpr std::uint64_t ITEMPICKUP_ITEM_OBJECT = 0xA8;
inline constexpr std::uint64_t ITEMPICKUP_SHORTNAME = 0xD8;
inline constexpr std::uint64_t ITEMPICKUP_AMOUNT = 0xE0;
inline constexpr std::uint64_t GAME_CONTROLLER_NET_IDENTITY_FIELD = 0x8;
inline constexpr std::uint64_t FPMANAGER_WORLD_FOV_TARGET = 0x9C;
inline constexpr std::uint64_t FPOBJECT_DRAW_EVENT = 0x20;
inline constexpr std::uint64_t FPDRAW_ACTION = 0x10;
inline constexpr std::uint64_t DELEGATE_TARGET = 0x20;
inline constexpr std::uint64_t MULTICAST_DELEGATES = 0x78;
inline constexpr std::uint64_t FPMOTION_OBJECT = 0xC8;
inline constexpr std::uint64_t FPMOTION_PLAYER = 0xE8;
inline constexpr std::uint64_t TRANSFORMOFFSET_POSITION = 0x24;
inline constexpr std::uint64_t TRANSFORMOFFSET_CURRENT_POSITION = 0x44;
inline constexpr std::uint64_t FPMOTION_IDLE_OFFSET = 0x78;
inline constexpr std::uint64_t FPMOTION_RUN_OFFSET = 0x80;
inline constexpr std::uint64_t FPMOTION_RUN_RELOAD_OFFSET = 0x88;
inline constexpr std::uint64_t FPMOTION_ATTACK_OFFSET = 0x90;
inline constexpr std::uint64_t FPMOTION_AIM_OFFSET = 0x98;
inline constexpr std::uint64_t FPMOTION_LADDER_OFFSET = 0xA0;
inline constexpr std::uint64_t FPMOTION_JUMP_OFFSET = 0xA8;
inline constexpr std::uint64_t FPMOTION_LOOK_OFFSET = 0xB0;
inline constexpr std::uint64_t FPMOTION_CLOSE_OFFSET = 0xB8;
inline constexpr std::uint64_t FPMOTION_ACTIVE_OFFSET = 0xD8;
inline constexpr std::uint64_t TRANSFORMOFFSET_LERP_SPEED = 0x20;

}  // namespace game_offsets_beta

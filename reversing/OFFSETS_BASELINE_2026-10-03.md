# Базовая карта оффсетов — релиз 2026-10-03

Этот файл фиксирует состояние перед следующим обновлением клиента. Он нужен для
сопоставления с новым `dump.cs`/`il2cpp.h`/`script.json`, а не как источник для
автоматического применения значений.

## Идентификатор базы

| Параметр | Значение |
|---|---|
| Срез | релиз, обновлённый 2026-10-03 |
| Файл с активными значениями | `jni/src/game_offsets.h` |
| SHA-256 файла оффсетов | `d741ae337946e148ec22b173c35eecce5e877e9b053463c0abf370a34b626cc0` |
| Compile-time констант | `255` |
| Runtime metadata | v39, `0x6dfaec8000`, `29441996` байт, канонический заголовок без XOR |
| `libil2cpp.so` исходного APK | `235607232` байт |
| Результат `il2cpp_dump.py` | `31704` типов; RVA у `258422` методов; `dump.cs`, `il2cpp.h`, `script.json` |
| Предыдущая база | `62a8534` (значения «было» сохранены в комментариях заголовка) |

Первичный источник этих сведений — шапка `game_offsets.h:4-19`. Исходные
`global-metadata.dat`, `libil2cpp.so`, `dump.cs`, `il2cpp.h` и `script.json`
релиза 2026-10-03 в checkout отсутствуют; здесь зафиксированы их
идентифицирующие параметры и уже проверенный результат анализа.

## Что есть в checkout

| Артефакт | Состояние | Идентификатор | Роль |
|---|---|---|---|
| `libunity.so` | есть | `25030176` байт, SHA-256 `0d4c182eb3e979e5683d01d26ecf3645c7a02cb59c5b040b5febec76153543f2` | нативные Unity Camera/Transform/GameObject оффсеты |
| `dump.log` | есть | `89743` байт, SHA-256 `09fb07ab34fba1a83e29061f645902640c80ba12bf86ef028371d4999c21e79d` | не является базовым runtime metadata: лог заканчивается `метадата не найдена` после скана `13857` МБ |
| `global-metadata.dat` базы | отсутствует | ожидался v39, `29441996` байт | managed layout и имена/порядок полей |
| `libil2cpp.so` базы | отсутствует | ожидался `235607232` байт | RVA, TypeInfo inner-slots, дизассемблерная сверка |
| `dump.cs` / `il2cpp.h` / `script.json` базы | отсутствуют | `31704` типов | структурное сопоставление на следующем билде |

`dump.log` начинается с `pid 29912`, но не находит ни одного metadata blob;
он не может быть источником текущих значений. Указанный в заголовке
runtime-снимок относится к отдельной сессии `benzhack` (`pid 7619` для
проверки TypeInfo), откуда в репозиторий попал только результат анализа.

## Метод, которым получены текущие значения

- **managed layout:** `dump.cs`/`il2cpp.h`; читаемые поля матчились по имени,
  обфусцированные — по типу, позиции, соседям и SyncVar-hook полям.
- **спорные managed-поля:** подтверждены дизассемблером arm64. Для следующего
  дампа нужно сохранять именно структурные якоря, а не прежние обфусцированные
  имена.
- **TypeInfo RVA:** из нового `libil2cpp.so` по `adrp/ldr -> outer GOT ->
  R_AARCH64_RELATIVE addend -> inner slot -> ldr [klass,#0xB8]`. В код идут
  именно inner-slot RVA. Для PlayerManager, PlayerLoopHelper, GameController
  и NetworkClient они дополнительно подтверждены живой сессией по имени
  `Il2CppClass`.
- **Unity native layout:** из `libunity.so`; Camera getter/helper пути
  зафиксированы у строк 31-55 исходного заголовка.

Новый билд нельзя обновлять единым delta: обфускатор меняет имена, добавляет
поля в середину классов и перекладывает `.data`/GOT. В первую очередь
сопоставляются тип, порядок и окружающие поля; затем метод/TypeInfo подтверждается
по новому ELF.

## Текущие константы

В таблице перечислены все `inline constexpr` из базовой версии. `group` —
функциональная группа по диапазону строк базового заголовка, `line` — строка
исходного файла на момент среза.

| line | symbol | value | type | group |
|---:|---|---|---|---|
| 38 | `CAMERA_PROJECTION_MATRIX` | `0xB0` | `std::uint64_t` | Unity Camera and shared object layouts |
| 39 | `CAMERA_VIEW_MATRIX` | `0x70` | `std::uint64_t` | Unity Camera and shared object layouts |
| 40 | `CAMERA_WORLD_TO_CLIP` | `0xF0` | `std::uint64_t` | Unity Camera and shared object layouts |
| 41 | `CAMERA_PREV_VIEW_PROJ` | `0x5C8` | `std::uint64_t` | Unity Camera and shared object layouts |
| 42 | `CAMERA_NATIVE_TRANSFORM` | `0x20` | `std::uint64_t` | Unity Camera and shared object layouts |
| 50 | `CAMERA_FOV_DEGREES` | `0x170` | `std::uint64_t` | Unity Camera and shared object layouts |
| 51 | `CAMERA_ASPECT` | `0x4E0` | `std::uint64_t` | Unity Camera and shared object layouts |
| 52 | `CAMERA_NEAR_CLIP` | `0x454` | `std::uint64_t` | Unity Camera and shared object layouts |
| 53 | `CAMERA_FAR_CLIP` | `0x458` | `std::uint64_t` | Unity Camera and shared object layouts |
| 54 | `CAMERA_VIEW_DIRTY` | `0x502` | `std::uint64_t` | Unity Camera and shared object layouts |
| 55 | `CAMERA_PROJ_DIRTY` | `0x500` | `std::uint64_t` | Unity Camera and shared object layouts |
| 57 | `MANAGED_CACHED_PTR` | `0x10` | `std::uint64_t` | Unity Camera and shared object layouts |
| 58 | `NETWORK_BEHAVIOUR_IDENTITY` | `0x40` | `std::uint64_t` | Unity Camera and shared object layouts |
| 59 | `NETWORK_IDENTITY_IS_LOCAL_PLAYER` | `0x4A` | `std::uint64_t` | Unity Camera and shared object layouts |
| 61 | `PLAYER_HEIGHT` | `1.8F` | `float` | Unity Camera and shared object layouts |
| 62 | `PLAYER_BOX_WIDTH_RATIO` | `0.40F` | `float` | Unity Camera and shared object layouts |
| 63 | `MIN_PLAYER_DISTANCE` | `0.0F` | `float` | Unity Camera and shared object layouts |
| 64 | `MAX_PLAYER_DISTANCE` | `300.0F` | `float` | Unity Camera and shared object layouts |
| 95 | `PLAYER_MANAGER_TYPEINFO_RVA` | `0xDCFD2A0` | `std::uint64_t` | Il2Cpp TypeInfo and static fields |
| 97 | `PLAYER_LOOP_HELPER_TYPEINFO_RVA` | `0xDCFD270` | `std::uint64_t` | Il2Cpp TypeInfo and static fields |
| 98 | `PLAYER_MANAGER_STATIC_FIELDS_LIST` | `0x10` | `std::uint64_t` | Il2Cpp TypeInfo and static fields |
| 102 | `IL2CPP_CLASS_STATIC_FIELDS_OFFSET` | `0xB8` | `std::uint64_t` | FP hitscan, reload and recoil |
| 103 | `PLAYERLOOPHELPER_RUNNERS` | `0x30` | `std::uint64_t` | FP hitscan, reload and recoil |
| 104 | `LOOPRUNNER_ITEMS` | `0x38` | `std::uint64_t` | FP hitscan, reload and recoil |
| 105 | `LOOPRUNNER_TAIL` | `0x30` | `std::uint64_t` | FP hitscan, reload and recoil |
| 106 | `DELAYPROMISE_TIMESPAN` | `0x18` | `std::uint64_t` | FP hitscan, reload and recoil |
| 107 | `DELAYPROMISE_ELAPSED` | `0x1C` | `std::uint64_t` | FP hitscan, reload and recoil |
| 108 | `DELAYPROMISE_INITIAL_FRAME` | `0x20` | `std::uint64_t` | FP hitscan, reload and recoil |
| 109 | `DELAYPROMISE_SCALED_TIMESPAN` | `0x1C` | `std::uint64_t` | FP hitscan, reload and recoil |
| 110 | `DELAYPROMISE_SCALED_ELAPSED` | `0x20` | `std::uint64_t` | FP hitscan, reload and recoil |
| 111 | `DELAYPROMISE_SCALED_INITIAL_FRAME` | `0x18` | `std::uint64_t` | FP hitscan, reload and recoil |
| 112 | `FPHITSCAN_CONFIG` | `0x130` | `std::uint64_t` | FP hitscan, reload and recoil |
| 115 | `FPWEAPON_EFFECTIVE_RELOAD_PERIOD` | `0xE8` | `std::uint64_t` | FP hitscan, reload and recoil |
| 116 | `FPWEAPON_SHOT_PERIOD` | `0x124` | `std::uint64_t` | FP hitscan, reload and recoil |
| 117 | `FPHITSCAN_BURST_WAIT` | `0x1F0` | `std::uint64_t` | FP hitscan, reload and recoil |
| 118 | `WAITFORSECONDS_SECONDS` | `0x10` | `std::uint64_t` | FP hitscan, reload and recoil |
| 119 | `FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD` | `0x2A0` | `std::uint64_t` | FP hitscan, reload and recoil |
| 120 | `FPHITSCAN_CALCULATOR` | `0x1F8` | `std::uint64_t` | FP hitscan, reload and recoil |
| 121 | `CALCULATOR_DISPERSION_CONFIG` | `0x10` | `std::uint64_t` | FP hitscan, reload and recoil |
| 122 | `CALCULATOR_HITSCAN_CONFIG` | `0x18` | `std::uint64_t` | FP hitscan, reload and recoil |
| 123 | `CALCULATOR_CACHED_BASE_DISPERSION` | `0x20` | `std::uint64_t` | FP hitscan, reload and recoil |
| 124 | `HITSCANCONFIG_FIRE_RATE` | `0x34` | `std::uint64_t` | FP hitscan, reload and recoil |
| 125 | `HITSCANCONFIG_FULL_RELOAD` | `0x48` | `std::uint64_t` | FP hitscan, reload and recoil |
| 126 | `HITSCANCONFIG_LOOP_RELOAD` | `0x4C` | `std::uint64_t` | FP hitscan, reload and recoil |
| 127 | `HITSCANCONFIG_DISPERSION_CONFIG` | `0x68` | `std::uint64_t` | FP hitscan, reload and recoil |
| 128 | `HITSCANCONFIG_RECOIL_CONFIG` | `0x70` | `std::uint64_t` | FP hitscan, reload and recoil |
| 129 | `FPHITSCAN_RECOIL` | `0x200` | `std::uint64_t` | FP hitscan, reload and recoil |
| 130 | `WEAPONRECOIL_RECOIL_CONFIG` | `0x00` | `std::uint64_t` | FP hitscan, reload and recoil |
| 131 | `WEAPONRECOIL_WEAPON_CONFIG` | `0x08` | `std::uint64_t` | FP hitscan, reload and recoil |
| 132 | `RECOILCONFIG_MIN_RECOIL` | `0x18` | `std::uint64_t` | FP hitscan, reload and recoil |
| 133 | `DISPERSIONCONFIG_MIN_DISPERSION` | `0x20` | `std::uint64_t` | FP hitscan, reload and recoil |
| 134 | `DISPERSIONCONFIG_MAX_DISPERSION` | `0x24` | `std::uint64_t` | FP hitscan, reload and recoil |
| 136 | `GAME_CONTROLLER_TYPEINFO_RVA` | `0xDCF8530` | `std::uint64_t` | GameController and CameraManager |
| 137 | `GAME_CONTROLLER_LOCAL_PLAYER_FIELD` | `0x10` | `std::uint64_t` | GameController and CameraManager |
| 138 | `GAME_CONTROLLER_CAMERA_MANAGER_FIELD` | `0x38` | `std::uint64_t` | GameController and CameraManager |
| 139 | `CAMERA_MANAGER_CAMERA_FIELD` | `0x20` | `std::uint64_t` | GameController and CameraManager |
| 142 | `PLAYER_TRANSFORM` | `0x68` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 146 | `PLAYER_POSITION` | `0x1F4` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 147 | `PLAYER_CHARACTER_MODEL` | `0x150` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 148 | `PLAYER_NICKLABEL` | `0x130` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 152 | `PLAYER_DISPLAY_NAME` | `0x240` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 155 | `NICKLABEL_PLAYER_BACKREF` | `0x20` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 156 | `NICKLABEL_NICKNAME_TEXT` | `0x38` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 158 | `UI_TEXT_MTEXT` | `0xE0` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 160 | `FPOBJECT_OBJECT_NAME` | `0x78` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 163 | `PLAYER_VOICE_STATE` | `0x328` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 164 | `VOICE_STATE_NAME` | `0x38` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 166 | `PLAYER_VOICE_PLAYER` | `0x140` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 167 | `VOICE_PLAYER_TAG` | `0x78` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 169 | `FPOBJECT_ITEM` | `0x40` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 170 | `ITEM_DATA` | `0x20` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 171 | `ITEMDATA_NAME` | `0x18` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 172 | `ITEMDATA_SHORTNAME` | `0x20` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 174 | `IL2CPP_STRING_LENGTH` | `0x10` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 175 | `IL2CPP_STRING_CHARS` | `0x14` | `std::uint64_t` | PlayerManager, names and held-item strings |
| 192 | `PLAYER_EVENT_HANDLER` | `0x78` | `std::uint64_t` | Player state, death, vitals and inventory |
| 193 | `EVENT_HANDLER_MANAGER_BACKREF` | `0xD0` | `std::uint64_t` | Player state, death, vitals and inventory |
| 194 | `EVENT_HANDLER_AIM_ACTIVITY` | `0x298` | `std::uint64_t` | Player state, death, vitals and inventory |
| 209 | `PLAYER_MANAGER_RESPAWNING` | `0x228` | `std::uint64_t` | Player state, death, vitals and inventory |
| 219 | `PLAYER_DEATH_HANDLER` | `0x80` | `std::uint64_t` | Player state, death, vitals and inventory |
| 220 | `DEATH_HANDLER_REF_TARGET` | `0x18` | `std::uint64_t` | Player state, death, vitals and inventory |
| 221 | `DEATH_HANDLER_DEATH` | `0xF0` | `std::uint64_t` | Player state, death, vitals and inventory |
| 227 | `PLAYER_VITALS` | `0xC8` | `std::uint64_t` | Player state, death, vitals and inventory |
| 228 | `VITALS_HEALTH` | `0xB8` | `std::uint64_t` | Player state, death, vitals and inventory |
| 229 | `PLAYER_INVENTORY` | `0x98` | `std::uint64_t` | Player state, death, vitals and inventory |
| 230 | `PLAYER_WEAPON_REFERENCE` | `0xF0` | `std::uint64_t` | Player state, death, vitals and inventory |
| 231 | `PLAYER_WEAPONS_ARRAY` | `0x198` | `std::uint64_t` | Player state, death, vitals and inventory |
| 233 | `INV_PLAYER_INVENTORY_DATA` | `0x20` | `std::uint64_t` | Player state, death, vitals and inventory |
| 234 | `INV_PLAYER_INVENTORY_CLIENT` | `0x28` | `std::uint64_t` | Player state, death, vitals and inventory |
| 252 | `PLAYERWEAPON_VIEW` | `0xE8` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 253 | `PLAYERWEAPON_PIECE` | `0x110` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 254 | `PLAYERWEAPON_STATE` | `0x120` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 255 | `PLAYERWEAPON_PLAYER_BACKREF` | `0x138` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 258 | `WEAPONPIECE_ENABLED` | `0x00` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 259 | `WEAPONPIECE_NUMBER` | `0x02` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 269 | `WEAPONVIEW_WEAPON_BASE` | `0x48` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 270 | `WEAPONVIEW_PIECE` | `0x50` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 271 | `WEAPONVIEW_ROOT_TRANSFORM` | `0x60` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 272 | `WEAPONVIEW_INNER` | `0x10` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 278 | `MODELINFO_RIGHT_WEAPON_HOLDER` | `0x28` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 279 | `MODELINFO_LEFT_WEAPON_HOLDER` | `0x30` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 282 | `CHARANIM_PLAYER_MODEL_INFO` | `0x30` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 283 | `INVDATA_PLAYER_MODEL_INFO` | `0x20` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 285 | `IL2CPP_CLASS_NAME` | `0x10` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 286 | `IL2CPP_CLASS_NAMESPACE` | `0x18` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 290 | `EVENT_HANDLER_LOOK_DIRECTION` | `0x140` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 293 | `EVENT_HANDLER_LAST_HIT_POINT` | `0x170` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 294 | `EVENT_HANDLER_LAST_HIT_TIME` | `0x178` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 295 | `SYNC_VALUE_OFFSET` | `0x20` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 296 | `ACTIVITY_ACTIVE_FLAG` | `0x10` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 297 | `PLAYER_FP_MANAGER` | `0x90` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 298 | `FPMANAGER_CURRENT_WEAPON` | `0x58` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 299 | `FPMANAGER_CURRENT_OBJECT` | `0x50` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 300 | `FPMANAGER_AIM_BLEND` | `0xA8` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 301 | `FPOBJECT_PLAYER_BACKREF` | `0xC0` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 302 | `FPWEAPON_IS_AIMING` | `0x120` | `std::uint64_t` | Remote weapon, ADS and aim direction |
| 321 | `FPOBJECT_RAYCAST_MANAGER` | `0x90` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 322 | `FPOBJECT_EVENT_HANDLER` | `0xC8` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 323 | `FPMELEE_MAX_REACH` | `0x128` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 324 | `FPMELEE_HIT_RADIUS` | `0x12C` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 329 | `FPMELEE_TIME_BETWEEN_ATTACKS` | `0x130` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 330 | `FPMELEE_PAUSE_AFTER_ATTACK` | `0x134` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 333 | `FPTOOL_TOOL_PURPOSES` | `0x160` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 336 | `RAYCASTMAN_PLAYER` | `0x20` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 337 | `RAYCASTMAN_RAY_LENGTH` | `0x38` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 338 | `RAYCASTMAN_AIM_RAY_LENGTH` | `0x3C` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 339 | `RAYCASTMAN_SPHERE_RADIUS` | `0x40` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 340 | `RAYCASTMAN_TOO_CLOSE` | `0x44` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 350 | `GUM_RAYCAST_DATA` | `0x160` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 351 | `GUM_AIM_RAYCAST` | `0x168` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 352 | `GUI_VALUE` | `0x20` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 353 | `GKO_HIT_OBJECT` | `0x18` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 354 | `GKO_RAYCAST_HIT` | `0x48` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 355 | `RAYCASTHIT_DISTANCE` | `0x1C` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 359 | `RAYCASTHIT_POINT` | `0x00` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 360 | `RAYCASTHIT_NORMAL` | `0x0C` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 365 | `RAYCASTHIT_COLLIDER` | `0x28` | `std::uint64_t` | Melee, RaycastManager and hit data |
| 374 | `PLAYER_KCC_REFERENCE` | `0xB0` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 375 | `KCC_PLAYER_BACKREF` | `0x78` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 376 | `KCC_HEAD_TRANSFORM` | `0x88` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 377 | `KCC_NORMAL_HEIGHT` | `0xA0` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 378 | `KCC_CROUCH_HEIGHT` | `0xA4` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 383 | `KCC_HITBOX_ROOT` | `0x70` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 384 | `HITBOX_ROOT_ARRAY` | `0x68` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 385 | `HITBOX_SIZE` | `0x24` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 386 | `HITBOX_CENTER` | `0x30` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 387 | `HITBOX_AREA` | `0x68` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 388 | `KCC_LOOK_HEIGHT_OFFSET` | `0x90` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 393 | `KCC_MOVE` | `0x13C` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 394 | `KCC_CHARACTER_ANIMATION` | `0xE0` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 395 | `CHAR_ANIM_PLAYER_BACKREF` | `0x78` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 396 | `CHAR_ANIM_RAGDOLL` | `0x38` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 397 | `RAGDOLL_PELVIS_RIGIDBODY` | `0x20` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 398 | `RAGDOLL_BONES_ARRAY` | `0x88` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 399 | `RAGDOLL_BODYPART_TRANSFORM` | `0x10` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 400 | `IL2CPP_ARRAY_LENGTH` | `0x18` | `std::uint64_t` | Ragdoll, KCC and player pose |
| 407 | `TRANSFORM_CHILDREN_ARRAY` | `0x48` | `std::uint64_t` | Native Unity Transform/GameObject and IL2CPP collections |
| 408 | `TRANSFORM_CHILD_COUNT` | `0x58` | `std::uint64_t` | Native Unity Transform/GameObject and IL2CPP collections |
| 410 | `TRANSFORM_HIERARCHY_DATA` | `0x38` | `std::uint64_t` | Native Unity Transform/GameObject and IL2CPP collections |
| 411 | `TRANSFORM_HIERARCHY_INDEX` | `0x40` | `std::uint64_t` | Native Unity Transform/GameObject and IL2CPP collections |
| 412 | `TRANSFORM_HIERARCHY_MATRICES` | `0x18` | `std::uint64_t` | Native Unity Transform/GameObject and IL2CPP collections |
| 413 | `TRANSFORM_HIERARCHY_INDICES` | `0x20` | `std::uint64_t` | Native Unity Transform/GameObject and IL2CPP collections |
| 414 | `COMPONENT_GAMEOBJECT` | `0x20` | `std::uint64_t` | Native Unity Transform/GameObject and IL2CPP collections |
| 415 | `GAMEOBJECT_COMPONENT_ARRAY` | `0x20` | `std::uint64_t` | Native Unity Transform/GameObject and IL2CPP collections |
| 416 | `COMPONENT_PAIR_PTR` | `0x08` | `std::uint64_t` | Native Unity Transform/GameObject and IL2CPP collections |
| 419 | `GAMEOBJECT_NAME` | `0x48` | `std::uint64_t` | Native Unity Transform/GameObject and IL2CPP collections |
| 422 | `GAMEOBJECT_NAME_GUESS` | `GAMEOBJECT_NAME` | `std::uint64_t` | Native Unity Transform/GameObject and IL2CPP collections |
| 424 | `IL2CPP_LIST_ITEMS` | `0x10` | `std::uint64_t` | Native Unity Transform/GameObject and IL2CPP collections |
| 425 | `IL2CPP_LIST_SIZE` | `0x18` | `std::uint64_t` | Native Unity Transform/GameObject and IL2CPP collections |
| 426 | `IL2CPP_ARRAY_FIRST_ELEMENT` | `0x20` | `std::uint64_t` | Native Unity Transform/GameObject and IL2CPP collections |
| 438 | `NETWORK_CLIENT_TYPEINFO_RVA` | `0xDCFC518` | `std::uint64_t` | Mirror world registry |
| 439 | `NETWORK_CLIENT_SPAWNED` | `0x30` | `std::uint64_t` | Mirror world registry |
| 444 | `DICT_ENTRIES` | `0x18` | `std::uint64_t` | Mirror world registry |
| 445 | `DICT_COUNT` | `0x20` | `std::uint64_t` | Mirror world registry |
| 446 | `DICT_ENTRY_STRIDE` | `0x18` | `std::uint64_t` | Mirror world registry |
| 447 | `DICT_ENTRY_VALUE` | `0x10` | `std::uint64_t` | Mirror world registry |
| 450 | `NETID_NET_ID` | `0x58` | `std::uint64_t` | Mirror world registry |
| 451 | `NETID_BEHAVIOURS` | `0x80` | `std::uint64_t` | Mirror world registry |
| 456 | `PMP_ENTITY` | `0x68` | `std::uint64_t` | Building vitals |
| 457 | `PMK_HEALTH` | `0x98` | `std::uint64_t` | Building vitals |
| 458 | `ARP_LATEST_VALUE` | `0x18` | `std::uint64_t` | Building vitals |
| 463 | `HEALTH_VALUE` | `0x20` | `std::uint64_t` | Building vitals |
| 464 | `VITALS_MAX_HEALTH` | `0x88` | `std::uint64_t` | Building vitals |
| 500 | `TOD_SCAN_RVA_BEGIN` | `0xDD00000` | `std::uint64_t` | TOD_Sky time of day |
| 501 | `TOD_SCAN_RVA_END` | `0xDDC0000` | `std::uint64_t` | TOD_Sky time of day |
| 502 | `TOD_SKY_CYCLE` | `0x40` | `std::uint64_t` | TOD_Sky time of day |
| 503 | `TOD_CYCLE_HOUR` | `0x10` | `std::uint64_t` | TOD_Sky time of day |
| 504 | `TOD_CYCLE_DAY` | `0x14` | `std::uint64_t` | TOD_Sky time of day |
| 505 | `TOD_CYCLE_MONTH` | `0x18` | `std::uint64_t` | TOD_Sky time of day |
| 506 | `TOD_CYCLE_YEAR` | `0x1C` | `std::uint64_t` | TOD_Sky time of day |
| 510 | `MINEABLE_LOOT` | `0xA0` | `std::uint64_t` | Mineable world entities |
| 511 | `MINEABLE_FINISH_BONUS` | `0xA8` | `std::uint64_t` | Mineable world entities |
| 512 | `MINEABLE_CURRENT_HEALTH` | `0x78` | `std::uint64_t` | Mineable world entities |
| 513 | `MINEABLE_HIT_ANCHOR` | `0xC8` | `std::uint64_t` | Mineable world entities |
| 515 | `LOOTITEM_ITEM_NAME` | `0x10` | `std::uint64_t` | Mineable world entities |
| 516 | `MINEABLE_MAX_HEALTH` | `0xC0` | `std::uint64_t` | Mineable world entities |
| 524 | `MINEABLE_FRACTION` | `0xD0` | `std::uint64_t` | Mineable world entities |
| 525 | `MINEABLE_ENTITY_TYPE` | `0xD8` | `std::uint64_t` | Mineable world entities |
| 530 | `MINEABLE_REQUIRED_TOOL_PURPOSE` | `0x70` | `std::uint64_t` | Mineable world entities |
| 532 | `MINEABLE_EXPERIENCE` | `0xD4` | `std::uint64_t` | Mineable world entities |
| 545 | `MINEABLE_EXTENSIONS` | `0xE8` | `std::uint64_t` | Mineable world entities |
| 567 | `OREHS_STREAK_INDEX` | `0x20` | `std::uint64_t` | Ore/tree hit-streak markers |
| 568 | `OREHS_MARKER_TEMPLATE` | `0x28` | `std::uint64_t` | Ore/tree hit-streak markers |
| 569 | `OREHS_MARKER` | `0x30` | `std::uint64_t` | Ore/tree hit-streak markers |
| 570 | `OREHS_COLLIDER` | `0x38` | `std::uint64_t` | Ore/tree hit-streak markers |
| 571 | `OREHS_MINEABLE` | `0x40` | `std::uint64_t` | Ore/tree hit-streak markers |
| 583 | `OREMARK_RENDERER` | `0x20` | `std::uint64_t` | Ore/tree hit-streak markers |
| 584 | `OREMARK_OWNER` | `0x38` | `std::uint64_t` | Ore/tree hit-streak markers |
| 585 | `OREMARK_SMOOTH` | `0x48` | `std::uint64_t` | Ore/tree hit-streak markers |
| 586 | `OREMARK_AGE` | `0x58` | `std::uint64_t` | Ore/tree hit-streak markers |
| 587 | `OREMARK_LIFETIME` | `15.0F` | `float` | Ore/tree hit-streak markers |
| 593 | `TREEHS_MARKER_TEMPLATE` | `0x28` | `std::uint64_t` | Ore/tree hit-streak markers |
| 594 | `TREEHS_STREAK` | `0x48` | `std::uint64_t` | Ore/tree hit-streak markers |
| 595 | `TREEHS_MARKER` | `0x50` | `std::uint64_t` | Ore/tree hit-streak markers |
| 596 | `TREEHS_SPOT_A` | `0x88` | `std::uint64_t` | Ore/tree hit-streak markers |
| 597 | `TREEHS_SPOT_B` | `0xA4` | `std::uint64_t` | Ore/tree hit-streak markers |
| 607 | `HITMARK_LIFETIME` | `0x20` | `std::uint64_t` | Ore/tree hit-streak markers |
| 608 | `HITMARK_FILTER` | `0x28` | `std::uint64_t` | Ore/tree hit-streak markers |
| 609 | `HITMARK_RENDERER` | `0x30` | `std::uint64_t` | Ore/tree hit-streak markers |
| 610 | `HITMARK_MARK` | `0x38` | `std::uint64_t` | Ore/tree hit-streak markers |
| 611 | `HITMARK_AGE` | `0xC8` | `std::uint64_t` | Ore/tree hit-streak markers |
| 626 | `PLAYER_USER_ID` | `0x298` | `std::uint64_t` | Player team, clan and vehicle SyncVars |
| 627 | `PLAYER_TEAM_NAME` | `0x2A0` | `std::uint64_t` | Player team, clan and vehicle SyncVars |
| 628 | `PLAYER_CLAN_ID` | `0x2B0` | `std::uint64_t` | Player team, clan and vehicle SyncVars |
| 629 | `PLAYER_CLAN_TAG` | `0x2B8` | `std::uint64_t` | Player team, clan and vehicle SyncVars |
| 634 | `PLAYER_VEHICLE_ID` | `0x2A8` | `std::uint64_t` | Player team, clan and vehicle SyncVars |
| 635 | `PLAYER_SEAT_ID` | `0x2AC` | `std::uint64_t` | Player team, clan and vehicle SyncVars |
| 641 | `LOOTOBJECT_INVENTORY` | `0xA0` | `std::uint64_t` | World loot containers |
| 642 | `LOOTOBJECT_IS_LOOTABLE` | `0xA8` | `std::uint64_t` | World loot containers |
| 643 | `LOOTOBJECT_PANEL_NAME` | `0xE0` | `std::uint64_t` | World loot containers |
| 647 | `LOOTOBJECT_BUILDING_PIECE` | `0xF8` | `std::uint64_t` | World loot containers |
| 653 | `ITEMPICKUP_ITEM_OBJECT` | `0xA8` | `std::uint64_t` | Ground pickups |
| 654 | `ITEMPICKUP_SHORTNAME` | `0xD8` | `std::uint64_t` | Ground pickups |
| 655 | `ITEMPICKUP_AMOUNT` | `0xE0` | `std::uint64_t` | Ground pickups |
| 659 | `GAME_CONTROLLER_NET_IDENTITY_FIELD` | `0x8` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 662 | `FPMANAGER_WORLD_FOV_TARGET` | `0x9C` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 663 | `FPOBJECT_DRAW_EVENT` | `0x20` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 664 | `FPDRAW_ACTION` | `0x10` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 665 | `DELEGATE_TARGET` | `0x20` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 666 | `MULTICAST_DELEGATES` | `0x78` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 667 | `FPMOTION_OBJECT` | `0xC8` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 668 | `FPMOTION_PLAYER` | `0xE8` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 669 | `TRANSFORMOFFSET_POSITION` | `0x24` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 670 | `TRANSFORMOFFSET_CURRENT_POSITION` | `0x44` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 671 | `FPMOTION_IDLE_OFFSET` | `0x78` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 672 | `FPMOTION_RUN_OFFSET` | `0x80` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 673 | `FPMOTION_RUN_RELOAD_OFFSET` | `0x88` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 674 | `FPMOTION_ATTACK_OFFSET` | `0x90` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 675 | `FPMOTION_AIM_OFFSET` | `0x98` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 676 | `FPMOTION_LADDER_OFFSET` | `0xA0` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 677 | `FPMOTION_JUMP_OFFSET` | `0xA8` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 678 | `FPMOTION_LOOK_OFFSET` | `0xB0` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 679 | `FPMOTION_CLOSE_OFFSET` | `0xB8` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 680 | `FPMOTION_ACTIVE_OFFSET` | `0xD8` | `std::uint64_t` | Viewmodel, delegates and FP motion |
| 683 | `TRANSFORMOFFSET_LERP_SPEED` | `0x20` | `std::uint64_t` | Viewmodel, delegates and FP motion |

## Следы предыдущих значений

Это не вторичный набор оффсетов: это только сохранённые в заголовке точные
старые значения и замечания об их замене.

- `game_offsets.h:19` — Прежние значения (дамп 62a8534) сохранены в комментариях «было».
- `game_offsets.h:95` — inline constexpr std::uint64_t PLAYER_MANAGER_TYPEINFO_RVA       = 0xDCFD2A0;  // ВНУТРЕННИЙ слот: outer 0xd9acc68 -> rela addend (раньше из-за опечатки брали outer 0xd9a8c68 -> 0xddb1ad8, чужой слот с мусором). Oxide.PlayerManager (TypeDefIndex 9624): методы LoL@0x691aac8, LmT@0x691aba8 и ещё 52 из 120 проверенных ссылаются на этот outer; рантайм: [слот]=0x6dd1b336b0=Il2CppClass{PlayerManager,Oxide}; через слот activePlayerList(+0x8)/clientPlayerList(+0x10)
- `game_offsets.h:119` — inline constexpr std::uint64_t FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD = 0x2A0;  // было 0x2E0; zrb: сеттер-инициализатор читает config[0x4C] (LoopReloadPeriod) и пишет в +0x2A0 — сверено дизассемблером
- `game_offsets.h:127` — inline constexpr std::uint64_t HITSCANCONFIG_DISPERSION_CONFIG = 0x68;  // было 0x60
- `game_offsets.h:128` — inline constexpr std::uint64_t HITSCANCONFIG_RECOIL_CONFIG = 0x70;  // было 0x68
- `game_offsets.h:146` — inline constexpr std::uint64_t PLAYER_POSITION  = 0x1F4;  // было 0x1D0; lastSavedPosition идёт вплотную за lastTickPosition(0x1E8), старый дамп говорил 0x1D4 при устройстве 0x1D0 — если на устройстве позиция «не та», попробовать 0x1F0
- `game_offsets.h:152` — inline constexpr std::uint64_t PLAYER_DISPLAY_NAME = 0x240;  // было 0x220; zNy (protected string) — прежний uWc; ВНИМАНИЕ: 0x220 теперь enum HackType lastViolationType, вставка +0x20 до animator
- `game_offsets.h:163` — inline constexpr std::uint64_t PLAYER_VOICE_STATE  = 0x328;  // было 0x2F8; zsd — единственное поле типа Dissonance.VoicePlayerState в классе (прежний uWr)
- `game_offsets.h:194` — inline constexpr std::uint64_t EVENT_HANDLER_AIM_ACTIVITY    = 0x298;  // было 0x290; класс переименован Gum->WxG (поля теперь с читаемыми именами: Run 0x280, Crouch 0x288, Jump 0x290, Aim 0x298 — вставка +0x8 перед Run)
- `game_offsets.h:209` — inline constexpr std::uint64_t PLAYER_MANAGER_RESPAWNING = 0x228;  // было 0x208; bool после nextTerrainPosition(0x224)
- `game_offsets.h:252` — inline constexpr std::uint64_t PLAYERWEAPON_VIEW           = 0xE8;  // было 0xE0; перед вьюхой вставлен YHw(bool)+паддинг; SyncVar-тройка не тронута (хуки _loaded/_weaponPiece/_weaponState на 0x150/0x158/0x160)
- `game_offsets.h:393` — inline constexpr std::uint64_t KCC_MOVE                   = 0x13C;  // было 0x154; luY (был ZYW): State +0x0, Pose +0x4, Aim +0x8, Position +0xC, RealVelocity +0x18
- `game_offsets.h:394` — inline constexpr std::uint64_t KCC_CHARACTER_ANIMATION     = 0xE0;  // было 0xF0; lua CharacterAnimation (был ZMl)
- `game_offsets.h:439` — inline constexpr std::uint64_t NETWORK_CLIENT_SPAWNED      = 0x30;  // было 0x28; перед spawned вставлен handlers(+0x28)
- `game_offsets.h:590` — (было TREEHS_MOVING_METHOD = 0x20 — поля MovingMethod в этом билде больше
- `game_offsets.h:626` — inline constexpr std::uint64_t PLAYER_USER_ID   = 0x298; // string userID; было 0x278
- `game_offsets.h:627` — inline constexpr std::uint64_t PLAYER_TEAM_NAME = 0x2A0; // string teamName; было 0x280
- `game_offsets.h:628` — inline constexpr std::uint64_t PLAYER_CLAN_ID   = 0x2B0; // string clanId; было 0x290
- `game_offsets.h:629` — inline constexpr std::uint64_t PLAYER_CLAN_TAG  = 0x2B8; // string clanTag; было 0x298
- `game_offsets.h:634` — inline constexpr std::uint64_t PLAYER_VEHICLE_ID = 0x2A8; // uint vehicleID; было 0x288
- `game_offsets.h:635` — inline constexpr std::uint64_t PLAYER_SEAT_ID    = 0x2AC; // uint seatID; было 0x28C


Семантические `enum class` (`ToolPurpose`, `MineableEntityType`) не входят в
счётчик 255: это значения протокола/игровой логики, а не адресные смещения.

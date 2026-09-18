# Откуда берётся каждый оффсет (PROVENANCE)

Живой срез для билда `62a8534` (предыдущий — `89e0b63`). Дампы лежат в
корне репозитория: `dump.7z` (dump.cs, il2cpp.h, script.json), `libil2cpp.7z`
и `libunity.7z`; распаковка — `bash tools/offsets/extract_dumps.sh <dir> [git-ref]`.

### Отпечаток текущего набора дампов

Первое, что делается с новыми дампами: сравнить эти цифры. Совпала
версия движка — класс C (рантайм/ABI) трогать не нужно, едут только поля
и RVA. Не совпала — пересчитывать всё, включая раскладку нативных
объектов Unity.

| что | значение |
|---|---|
| версия Unity | **6000.3.18f1** (из строк libil2cpp.so/libunity.so) |
| версия метаданных | v39 (шапка dump.cs) |
| таблицы | типов 31138, методов 275631, полей 136787 |
| `dump.cs` | 137920389 байт, sha1 `509671dee2f9…` |
| `il2cpp.h` | 218831888 байт, sha1 `2b6da92bd8ad…` |
| `script.json` | 232773073 байт, sha1 `bada792c23e1…` |
| `libil2cpp.so` | 231223744 байт, sha1 `b78938706056…` |
| `libunity.so` | 24919632 байт, sha1 `354e4084943e…` |


Машиночитаемый источник истины — `tools/offsets/offsets_map.json`
(186 записей); этот файл — его человеческое объяснение: не «какое число
стоит», а «откуда оно взялось и как его найти заново». Значения здесь
совпадают с `jni/src/game_offsets.h`; при расхождении верить заголовку и
карте, а этот файл пересобрать (`make_provenance.py`).

Константы делятся на три класса — и обновляются они по-разному:

| класс | сколько | откуда | уезжает ли каждый билд | чем проверяется |
|---|---|---|---|---|
| **A. Смещения полей** | 141 | раскладка структур в `il2cpp.h` (`/* 0xNN */`) | да, если в класс добавили/убрали поле | `update_offsets.py` автоматически |
| **B. `*_TYPEINFO_RVA`** | 3 | слоты глобальных `Il2CppClass*` в `.data.rel.ro` `libil2cpp.so` | **да, всегда** | `typeinfo_rva.py` + отпечаток со старого дампа |
| **C. Рантайм/ABI** | 41 | раскладка Unity/IL2CPP-объектов, не выводится из `dump.cs` | только при смене версии Unity/IL2CPP | вручную (дизасм паттернов), см. §C |

Отдельно — не константы, но тоже привязано к билду: имена классов (сверяются
в рантайме со строкой `Il2CppClass.name`) и окно скана `TOD_SCAN_RVA_*`.

---

## A. Смещения полей (`il2cpp.h`)

Как читать: `структура` — имя из `il2cpp.h` (`<Класс>_Fields`), `поле` — имя в
**этом** билде (`62a8534`). Обфусцированные имена ротируют каждый билд
(`MoW`→`lzD`), поэтому искать поле надо по смещению+типу, а не по имени;
скрипт так и делает (сначала имя, если оно читаемое, иначе позиционное
выравнивание последовательности типов старой структуры на новую).

Часть констант живёт не в именованной структуре, а в классе ПО указателю
из неё (`PlayerManager.playerEventHandler` → `Aim`), и имя того класса
тоже ротирует (`DqO`→`Gum`). В карте такие записи несут `via` — путь от
стабильной структуры по читаемым именам полей; `update_offsets.py`
разрешает его в имена структур обоих дампов и проверяет константу как
обычное поле. Без `via` проверка вырождается в «у `PlayerManager` есть
поле по 0x268» — это верно всегда, и сдвиг внутри самого хендлера
проходит молча. Так и вышло в билде `62a8534`: туда вставили `KnockDoor`
(0x188), всё после него уехало на +8, `Aim` сместился 0x268→0x270, а по
0x268 встал `Jump` — переключатель «Только в прицеле» читал флаг прыжка.

### `Oxide_PlayerManager_StaticFields`  (PlayerManager.static)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `PLAYER_MANAGER_STATIC_FIELDS_LIST` | 0x10 | `clientPlayerList` | `Il2CppObject*` |

### `Oxide_GameControllerBase_StaticFields`  (GameControllerBase.static)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `GAME_CONTROLLER_NET_IDENTITY_FIELD` | 0x8 | `_LPG_k__BackingField` | `Mirror_NetworkIdentity_o*` |
| `GAME_CONTROLLER_LOCAL_PLAYER_FIELD` | 0x10 | `_LPj_k__BackingField` | `Oxide_PlayerManager_o*` |
| `GAME_CONTROLLER_CAMERA_MANAGER_FIELD` | 0x38 | `_LPU_k__BackingField` | `Oxide_CameraManager_o*` |

### `Oxide_CameraManager_Fields`  (CameraManager)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `CAMERA_MANAGER_CAMERA_FIELD` | 0x20 | `m_Camera` | `UnityEngine_Camera_o*` |

### `Oxide_PlayerManager_Fields`  (PlayerManager)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `PLAYER_TRANSFORM` | 0x68 | `worldCameraRoot` | `UnityEngine_Transform_o*` |
| `PLAYER_EVENT_HANDLER` | 0x78 | `playerEventHandler` | `Gum_o*` |
| `PLAYER_FP_MANAGER` | 0x90 | `fpManager` | `Oxide_FPManager_o*` |
| `PLAYER_INVENTORY` | 0x98 | `inventory` | `Oxide_PlayerInventory_o*` |
| `PLAYER_KCC_REFERENCE` | 0xb0 | `kccReference` | `Il2CppObject*` |
| `PLAYER_WEAPON_REFERENCE` | 0xf0 | `weaponReference` | `Il2CppObject*` |
| `PLAYER_NICKLABEL` | 0x130 | `nicklabel` | `oh_o*` |
| `PLAYER_VOICE_PLAYER` | 0x140 | `voicePlayer` | `Gmv_o*` |
| `PLAYER_CHARACTER_MODEL` | 0x150 | `characterModel` | `UnityEngine_GameObject_o*` |
| `PLAYER_WEAPONS_ARRAY` | 0x198 | `weapons` | `Il2CppObject*` |
| `PLAYER_POSITION` | 0x1d4 | `lastSavedPosition` | `UnityEngine_Vector3_o` |
| `PLAYER_DISPLAY_NAME` | 0x220 | `LWQ` | `System_String_o*` |
| `PLAYER_USER_ID` | 0x278 | `userID` | `System_String_o*` |
| `PLAYER_TEAM_NAME` | 0x280 | `teamName` | `System_String_o*` |
| `PLAYER_VEHICLE_ID` | 0x288 | `vehicleID` | `uint32_t` |
| `PLAYER_SEAT_ID` | 0x28c | `seatID` | `uint32_t` |
| `PLAYER_CLAN_ID` | 0x290 | `clanId` | `System_String_o*` |
| `PLAYER_CLAN_TAG` | 0x298 | `clanTag` | `System_String_o*` |
| `PLAYER_VOICE_STATE` | 0x2e8 | `Llp` | `Dissonance_VoicePlayerState_o*` |

### `oh_Fields`  (nicklabel widget (имя ротирует: OS -> oh))

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `NICKLABEL_PLAYER_BACKREF` | 0x20 | `player` | `Oxide_PlayerManager_o*` |
| `NICKLABEL_NICKNAME_TEXT` | 0x38 | `nickname` | `UnityEngine_UI_Text_o*` |

### `UnityEngine_UI_Text_Fields`  (UI.Text)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `UI_TEXT_MTEXT` | 0xe0 | `m_Text` | `System_String_o*` |

### `Oxide_FPObject_Fields`  (FPObject)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `FPOBJECT_ITEM` | 0x40 | `_Lwx_k__BackingField` | `Oxide_Item_o*` |
| `FPOBJECT_OBJECT_NAME` | 0x78 | `m_ObjectName` | `System_String_o*` |
| `FPOBJECT_RAYCAST_MANAGER` | 0x90 | `LUw` | `Oxide_RaycastManager_o*` |
| `FPOBJECT_PLAYER_BACKREF` | 0xc0 | `Player` | `Oxide_PlayerManager_o*` |
| `FPOBJECT_EVENT_HANDLER` | 0xc8 | `PlayerEventHandler` | `Gum_o*` |

### `Dissonance_VoicePlayerState_Fields`  (VoicePlayerState)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `VOICE_STATE_NAME` | 0x38 | `_Name_k__BackingField` | `System_String_o*` |

### `Gmv_Fields`  (VoicePlayer (имя ротирует: DEY -> Gmv))

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `VOICE_PLAYER_TAG` | 0x78 | `ccW` | `System_String_o*` |

### `Oxide_Item_Fields`  (Item)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `ITEM_DATA` | 0x20 | `_LSg_k__BackingField` | `Oxide_ItemData_o*` |

### `Oxide_ItemData_Fields`  (ItemData)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `ITEMDATA_NAME` | 0x18 | `m_Name` | `System_String_o*` |
| `ITEMDATA_SHORTNAME` | 0x20 | `m_ShortName` | `System_String_o*` |

### `Gum_Fields`  (PlayerEventHandler (имя ротирует: DqO -> Gum))

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `EVENT_HANDLER_MANAGER_BACKREF` | 0xd0 | `manager` | `Oxide_PlayerManager_o*` |
| `EVENT_HANDLER_LOOK_DIRECTION` | 0x140 | `LookDirection` | `Il2CppObject*` |
| `GUM_RAYCAST_DATA` | 0x160 | `RaycastData` | `Il2CppObject*` |
| `GUM_AIM_RAYCAST` | 0x168 | `AimRaycast` | `Il2CppObject*` |
| `EVENT_HANDLER_LAST_HIT_POINT` | 0x170 | `LastLocalHitPoint` | `Il2CppObject*` |
| `EVENT_HANDLER_LAST_HIT_TIME` | 0x178 | `LastLocalHitTime` | `Il2CppObject*` |
| `EVENT_HANDLER_AIM_ACTIVITY` | 0x270 | `Aim` | `Gub_o*` |

### `Oxide_PlayerInventory_Fields`  (PlayerInventory)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `INV_PLAYER_INVENTORY_DATA` | 0x20 | `_playerInventoryData` | `Oxide_PlayerInventoryData_o*` |
| `INV_PLAYER_INVENTORY_CLIENT` | 0x28 | `_playerInventoryClient` | `GqT_o*` |

### `HyperHug_Games_Oxide_Features_Weapons_PlayerWeapon_Fields`  (PlayerWeapon)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `PLAYERWEAPON_VIEW` | 0xe0 | `playerWeaponViewReference` | `bp_o*` |
| `PLAYERWEAPON_PIECE` | 0x110 | `CxH` | `Oxide_WeaponPiece_o` |
| `PLAYERWEAPON_STATE` | 0x120 | `Cxn` | `int32_t` |
| `PLAYERWEAPON_PLAYER_BACKREF` | 0x138 | `_player_k__BackingField` | `Oxide_PlayerManager_o*` |

### `Oxide_WeaponPiece_Fields`  (WeaponPiece)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `WEAPONPIECE_ENABLED` | 0x0 | `Enabled` | `bool` |
| `WEAPONPIECE_NUMBER` | 0x2 | `Number` | `int16_t` |

### `HyperHug_Games_Oxide_Features_Player_PlayerModelInfo_Fields`  (PlayerModelInfo)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `INVDATA_PLAYER_MODEL_INFO` | 0x20 | `head` | `UnityEngine_Transform_o*` |
| `MODELINFO_RIGHT_WEAPON_HOLDER` | 0x28 | `rightWeaponHolder` | `UnityEngine_Transform_o*` |
| `MODELINFO_LEFT_WEAPON_HOLDER` | 0x30 | `leftWeaponHolder` | `UnityEngine_Transform_o*` |
| `CHARANIM_PLAYER_MODEL_INFO` | 0x30 | `leftWeaponHolder` | `UnityEngine_Transform_o*` |

### `Gub_Fields`  (Activity (имя ротирует: Dqg -> Gub))

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `ACTIVITY_ACTIVE_FLAG` | 0x10 | `_LEb_k__BackingField` | `bool` |

### `Oxide_FPManager_Fields`  (FPManager)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `FPMANAGER_CURRENT_OBJECT` | 0x50 | `_currentWeapon` | `Oxide_FPObject_o*` |
| `FPMANAGER_CURRENT_WEAPON` | 0x58 | `LwC` | `Oxide_FPWeaponBase_o*` |
| `FPMANAGER_AIM_BLEND` | 0xa8 | `_Lwv_k__BackingField` | `float` |

### `Oxide_FPWeaponBase_Fields`  (FPWeaponBase)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `FPWEAPON_IS_AIMING` | 0x120 | `_LUD_k__BackingField` | `bool` |

### `HyperHug_Games_Oxide_Features_Player_KCC_Fields`  (KCC)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `KCC_HITBOX_ROOT` | 0x70 | `hitBoxRecorderRoot` | `HyperHug_Games_Oxide_Features_Network_HitBoxRecorderRoot_o*` |
| `KCC_PLAYER_BACKREF` | 0x78 | `player` | `Oxide_PlayerManager_o*` |
| `KCC_HEAD_TRANSFORM` | 0x88 | `head` | `UnityEngine_Transform_o*` |
| `KCC_LOOK_HEIGHT_OFFSET` | 0x90 | `lookHeightOffset` | `float` |
| `KCC_NORMAL_HEIGHT` | 0xa0 | `normalHeight` | `float` |
| `KCC_CROUCH_HEIGHT` | 0xa4 | `m_CrouchHeight` | `float` |
| `KCC_CHARACTER_ANIMATION` | 0x108 | `lqU` | `HyperHug_Games_Oxide_Features_Player_CharacterAnimation_o*` |
| `KCC_MOVE` | 0x16c | `lql` | `HyperHug_Games_Oxide_Features_Player_Move_o` |

### `Oxide_HitBox_Fields`  (HitBox)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `HITBOX_SIZE` | 0x24 | `size` | `UnityEngine_Vector3_o` |
| `HITBOX_CENTER` | 0x30 | `center` | `UnityEngine_Vector3_o` |
| `HITBOX_ROOT_ARRAY` | 0x68 | `m_HitArea` | `int32_t` |
| `HITBOX_AREA` | 0x68 | `m_HitArea` | `int32_t` |

### `HyperHug_Games_Oxide_Features_Player_CharacterAnimation_Fields`  (CharacterAnimation)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `CHAR_ANIM_RAGDOLL` | 0x38 | `ragdoll` | `HyperHug_Games_Oxide_By_Namespace_Oxide_Damage_System_Ragdoll_o*` |
| `CHAR_ANIM_PLAYER_BACKREF` | 0x78 | `lcI` | `Oxide_PlayerManager_o*` |

### `HyperHug_Games_Oxide_By_Namespace_Oxide_Damage_System_Ragdoll_Fields`  (Ragdoll)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `RAGDOLL_PELVIS_RIGIDBODY` | 0x20 | `m_Pelvis` | `UnityEngine_Rigidbody_o*` |
| `RAGDOLL_BONES_ARRAY` | 0x88 | `m_Bones` | `Il2CppObject*` |

### `HyperHug_Games_Oxide_By_Namespace_Oxide_Damage_System_Ragdoll_BodyPart_Fields`  (Ragdoll.BodyPart)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `RAGDOLL_BODYPART_TRANSFORM` | 0x10 | `transform` | `UnityEngine_Transform_o*` |

### `Mirror_NetworkClient_StaticFields`  (NetworkClient.static)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `NETWORK_CLIENT_SPAWNED` | 0x28 | `spawned` | `Il2CppObject*` |

### `Mirror_NetworkIdentity_Fields`  (NetworkIdentity)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `DICT_COUNT` | 0x20 | `canFrequencyBeChanged` | `bool` |
| `NETID_NET_ID` | 0x58 | `_netId_k__BackingField` | `uint32_t` |
| `NETID_BEHAVIOURS` | 0x80 | `_NetworkBehaviours_k__BackingField` | `Il2CppObject*` |

### `Oxide_MineableObject_Fields`  (MineableObject)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `MINEABLE_REQUIRED_TOOL_PURPOSE` | 0x70 | `m_RequiredToolPurpose` | `int32_t` |
| `MINEABLE_CURRENT_HEALTH` | 0x78 | `m_CurrentHealth` | `float` |
| `MINEABLE_LOOT` | 0xa0 | `m_Loot` | `Il2CppObject*` |
| `MINEABLE_FINISH_BONUS` | 0xa8 | `m_FinishBonus` | `Il2CppObject*` |
| `MINEABLE_MAX_HEALTH` | 0xc0 | `m_MaxHealth` | `float` |
| `MINEABLE_HIT_ANCHOR` | 0xc8 | `LXX` | `UnityEngine_Transform_o*` |
| `MINEABLE_FRACTION` | 0xd0 | `fractionRemaining` | `float` |
| `MINEABLE_EXPERIENCE` | 0xd4 | `m_Experience` | `int32_t` |
| `MINEABLE_ENTITY_TYPE` | 0xd8 | `entityType` | `int32_t` |
| `MINEABLE_EXTENSIONS` | 0xe8 | `LXZ` | `Il2CppObject*` |

### `Oxide_LootItem_Fields`  (LootItem)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `LOOTITEM_ITEM_NAME` | 0x10 | `ItemName` | `System_String_o*` |

### `Oxide_LootObject_Fields`  (LootObject)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `LOOTOBJECT_INVENTORY` | 0xa0 | `inventory` | `Oxide_Inventory_o*` |
| `LOOTOBJECT_IS_LOOTABLE` | 0xa8 | `isLootable` | `bool` |
| `LOOTOBJECT_PANEL_NAME` | 0xe0 | `panelName` | `System_String_o*` |
| `LOOTOBJECT_BUILDING_PIECE` | 0xf8 | `m_Piece` | `Oxide_Building_BuildingPiece_o*` |

### `Oxide_ItemPickup_Fields`  (ItemPickup)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `ITEMPICKUP_ITEM_OBJECT` | 0xa8 | `_Ltk_k__BackingField` | `Oxide_Item_o*` |
| `ITEMPICKUP_SHORTNAME` | 0xd8 | `item` | `System_String_o*` |
| `ITEMPICKUP_AMOUNT` | 0xe0 | `amount` | `int32_t` |

### `HyperHug_Games_Oxide_Features_MineableExtensions_MineableObjectExtension_OreHitstreaks_Fields`  (MineableObjectExtension_OreHitstreaks)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `OREHS_STREAK_INDEX` | 0x20 | `hitstreakIndex` | `int32_t` |
| `OREHS_MARKER_TEMPLATE` | 0x28 | `hitStreakMarker` | `HyperHug_Games_Oxide_Features_MineableExtensions_MineableObjectExtension_OreHitstreaksMarker_o*` |
| `OREHS_MARKER` | 0x30 | `lzD` | `HyperHug_Games_Oxide_Features_MineableExtensions_MineableObjectExtension_OreHitstreaksMarker_o*` |
| `OREHS_COLLIDER` | 0x38 | `lzR` | `UnityEngine_Collider_o*` |
| `OREHS_MINEABLE` | 0x40 | `lzk` | `Oxide_MineableObject_o*` |

### `HyperHug_Games_Oxide_Features_MineableExtensions_MineableObjectExtension_OreHitstreaksMarker_Fields`  (MineableObjectExtension_OreHitstreaksMarker)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `OREMARK_RENDERER` | 0x20 | `meshRenderer` | `UnityEngine_MeshRenderer_o*` |
| `OREMARK_OWNER` | 0x38 | `lzF` | `HyperHug_Games_Oxide_Features_MineableExtensions_MineableObjectExtension_OreHitstreaks_o*` |
| `OREMARK_SMOOTH` | 0x48 | `lzA` | `float` |
| `OREMARK_AGE` | 0x58 | `lHG` | `float` |

### `HyperHug_Games_Oxide_Features_MineableExtensions_MineableObjectExtension_TreeHitstreaks_Fields`  (MineableObjectExtension_TreeHitstreaks)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `TREEHS_MOVING_METHOD` | 0x20 | `movingMethod` | `int32_t` |
| `TREEHS_MARKER_TEMPLATE` | 0x28 | `hitStreakMarkerOriginal` | `HyperHug_Games_Oxide_Features_MineableExtensions_MineableObjectExtension_HitMarkerItem_o*` |
| `TREEHS_STREAK` | 0x48 | `lHX` | `int32_t` |
| `TREEHS_MARKER` | 0x50 | `lHe` | `HyperHug_Games_Oxide_Features_MineableExtensions_MineableObjectExtension_HitMarkerItem_o*` |
| `TREEHS_SPOT_A` | 0x88 | `lHC` | `UnityEngine_Vector3_o` |
| `TREEHS_SPOT_B` | 0xa4 | `lHl` | `UnityEngine_Vector3_o` |

### `HyperHug_Games_Oxide_Features_MineableExtensions_MineableObjectExtension_HitMarkerItem_Fields`  (MineableObjectExtension_HitMarkerItem)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `HITMARK_LIFETIME` | 0x20 | `lifetime` | `float` |
| `HITMARK_FILTER` | 0x28 | `mFilter` | `UnityEngine_MeshFilter_o*` |
| `HITMARK_RENDERER` | 0x30 | `renderer` | `UnityEngine_Renderer_o*` |
| `HITMARK_MARK` | 0x38 | `mark` | `UnityEngine_Transform_o*` |
| `HITMARK_AGE` | 0xc8 | `lzr` | `float` |

### `TOD_CycleParameters_Fields`  (TOD_CycleParameters)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `TOD_CYCLE_HOUR` | 0x10 | `Hour` | `float` |
| `TOD_CYCLE_DAY` | 0x14 | `Day` | `int32_t` |
| `TOD_CYCLE_MONTH` | 0x18 | `Month` | `int32_t` |
| `TOD_CYCLE_YEAR` | 0x1c | `Year` | `int32_t` |

### `Oxide_FPMelee_Fields`  (FPMelee)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `FPMELEE_MAX_REACH` | 0x128 | `m_MaxReach` | `float` |
| `FPMELEE_HIT_RADIUS` | 0x12c | `hitRadius` | `float` |
| `FPMELEE_TIME_BETWEEN_ATTACKS` | 0x130 | `m_TimeBetweenAttacks` | `float` |
| `FPMELEE_PAUSE_AFTER_ATTACK` | 0x134 | `pauseAfterAttack` | `float` |

### `Oxide_FPTool_Fields`  (FPTool)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `FPTOOL_TOOL_PURPOSES` | 0x160 | `m_ToolPurposes` | `int32_t` |

### `Oxide_RaycastManager_Fields`  (RaycastManager)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `RAYCASTMAN_PLAYER` | 0x20 | `player` | `Oxide_PlayerManager_o*` |
| `RAYCASTMAN_RAY_LENGTH` | 0x38 | `m_RayLength` | `float` |
| `RAYCASTMAN_AIM_RAY_LENGTH` | 0x3c | `m_AimRayLength` | `float` |
| `RAYCASTMAN_SPHERE_RADIUS` | 0x40 | `LtS` | `float` |
| `RAYCASTMAN_TOO_CLOSE` | 0x44 | `m_TooCloseThreeshold` | `float` |

### `GKo_Fields`  (результат луча (имя ротирует; ищется по форме: RaycastHit на 0x48 и GameObject на 0x18))

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `GKO_HIT_OBJECT` | 0x18 | `_Ltj_k__BackingField` | `UnityEngine_GameObject_o*` |
| `GKO_RAYCAST_HIT` | 0x48 | `_Ltt_k__BackingField` | `UnityEngine_RaycastHit_o` |

### `UnityEngine_RaycastHit_Fields`  (UnityEngine.RaycastHit)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `RAYCASTHIT_POINT` | 0x0 | `m_Point` | `UnityEngine_Vector3_o` |
| `RAYCASTHIT_NORMAL` | 0xc | `m_Normal` | `UnityEngine_Vector3_o` |
| `RAYCASTHIT_DISTANCE` | 0x1c | `m_Distance` | `float` |
| `RAYCASTHIT_COLLIDER` | 0x28 | `m_Collider` | `UnityEngine_EntityId_o` |

### `Oxide_GenericVitals_Fields`  (Oxide_GenericVitals_Fields)

| константа | offset | поле в билде `62a8534` | тип |
|---|---|---|---|
| `VITALS_MAX_HEALTH` | 0x88 | `m_MaxHealth` | `float` |

---

## B. `*_TYPEINFO_RVA` — слоты глобальных `Il2CppClass*`

Ридер не может найти класс по имени (метадата-таблицы в `.so` нет,
`ScriptMetadata` в `script.json` пустой), поэтому читает готовый указатель из
`.data.rel.ro`: `rd_ptr(base + RVA)` → `Il2CppClass*`, дальше имя класса
(`+0x10`/`+0x18`) для контроля и `static_fields` (`+0xB8`).

| константа | RVA | класс | отпечаток (как отличить от чужого слота) |
|---|---|---|---|
| `PLAYER_MANAGER_TYPEINFO_RVA` | 0xD8DB8B8 | `Oxide.PlayerManager` | 5 обращений, статик-поля `[0x1A0x1]` |
| `GAME_CONTROLLER_TYPEINFO_RVA` | 0xD8D61E8 | `Oxide.GameControllerBase` | 1 обращение, статик-полей нет — **верхний кандидат здесь всегда чужой** |
| `NETWORK_CLIENT_TYPEINFO_RVA` | 0xD8DAB08 | `Mirror.NetworkClient` | 6 обращений, статик-полей нет |

Как найти заново (оба прогона обязательны):

```bash
# 1) старый дамп: скрипт обязан воспроизвести значения, которые сейчас в git
python3 tools/offsets/typeinfo_rva.py --so <old>/libil2cpp.so --script <old>/script.json \
        --methods 3000 --top 6 Oxide.PlayerManager Oxide.GameControllerBase Mirror.NetworkClient
# 2) новый дамп: берём кандидат С ТЕМ ЖЕ отпечатком, а не верхнего
python3 tools/offsets/typeinfo_rva.py --so <new>/libil2cpp.so --script <new>/script.json \
        --methods 3000 --top 6 Oxide.PlayerManager Oxide.GameControllerBase Mirror.NetworkClient
```

`--methods 3000` обязателен: на 400 методах у `PlayerManager` в билде `62a8534`
не находится ни одного кандидата. `update_offsets.py` делает оба прогона сам и
подбирает по отпечатку; если отпечаток не совпал — предупреждает, и тогда
решать вручную.

---

## C. Рантайм/ABI — не из `dump.cs`

Раскладка нативных объектов Unity и служебных структур IL2CPP. В дампе игры её
нет, поэтому значения получены дизассемблером/рантаймом и меняются только при
смене версии Unity. `update_offsets.py` их не проверяет (в карте `kind:
runtime`) — после крупного апдейта движка сверять руками.

| константа | значение | что это | где искать в дампах |
|---|---|---|---|
| `PMK_HEALTH` | 0x98 | НЕ ИСПОЛЬЗУЕТСЯ. AsyncReactiveProperty<float> здоровья сущности | ? (класс ротирует) — не используется в коде. Если понадобится: в том же классе, что PMP_ENTITY, поле типа AsyncReactiveProperty<float> — проверить по 0x98 |
| `PMP_ENTITY` | 0x68 | НЕ ИСПОЛЬЗУЕТСЯ. При обновлении можно не переносить; если понадобится — искать класс, у которого на 0x98 лежит AsyncReactiveProperty_1_o* | ? (класс ротирует) — не используется в коде (только в переключателе версий). Класс «pmK» в текущем дампе не опознан: имя обфусцировано и сменилось |
| `MANAGED_CACHED_PTR` | 0x10 | UnityEngine.Object.m_CachedPtr: managed-обёртка -> нативный объект | ABI + il2cpp.h — UnityEngine.Object: за заголовком объекта (klass 0x0, monitor 0x8) первым идёт m_CachedPtr -> 0x10; проверено на всех managed-обёртках |
| `IL2CPP_ARRAY_FIRST_ELEMENT` | 0x20 | Il2CppArray: первый элемент (данные начинаются отсюда) | ABI il2cpp — данные массива начинаются с 0x20 |
| `IL2CPP_ARRAY_LENGTH` | 0x18 | Il2CppArray.max_length (int32) | ABI il2cpp — Il2CppArray: klass 0x0, monitor 0x8, bounds* 0x10, max_length 0x18, данные 0x20 |
| `IL2CPP_STRING_CHARS` | 0x14 | System.String: первый символ (UTF-16) | il2cpp.h — struct System_String_Fields: _firstChar /* 0x14 */; ПРОВЕРЕНО по дампу |
| `IL2CPP_STRING_LENGTH` | 0x10 | System.String.length (UTF-16) | il2cpp.h — struct System_String_Fields: _stringLength /* 0x10 */; ПРОВЕРЕНО по дампу |
| `ARP_LATEST_VALUE` | 0x18 | AsyncReactiveProperty<T>.latestValue — реактивное свойство (здоровье у сущностей) | il2cpp.h + ABI — Cysharp_Threading_Tasks_AsyncReactiveProperty_1_Fields: порядок (triggerEvent, latestValue) плюс заголовок 0x10 -> 0x18 |
| `DICT_ENTRIES` | 0x18 | Dictionary<K,V>._entries (массив struct Entry) | il2cpp.h + ABI — System_Collections_Generic_Dictionary_2_TKey_TValue__Fields: порядок (_buckets, _entries, ...) плюс заголовок 0x10 -> 0x18 |
| `DICT_ENTRY_STRIDE` | 0x18 | размер struct Entry {hashCode, next, key, value} | il2cpp.h + ABI — Entry{TKey,TValue}: (hashCode i4, next i4, key ptr, value ptr) = 0x18. Внутри массива заголовка объекта нет — это важно, иначе stride был бы 0x28 |
| `DICT_ENTRY_VALUE` | 0x10 | смещение value внутри struct Entry | il2cpp.h + ABI — value — четвёртое поле Entry: 0x0+4+8 = 0x10 |
| `GUI_VALUE` | 0x20 | значение внутри обёртки GuI`1<T>: в il2cpp.h у GuI_1_Fields полей нет (generic), смещение подтверждено дизассемблером FPMelee.ZkX 0x6533a20 — ldr x8,[x19,#0xc8]; ldr x8,[x8,#0x160]; ldr x20,[x8,#0x20] | il2cpp.h + ABI — обёртка GuI`1<T>: в il2cpp.h у GuI_1_Fields полей нет (generic), 0x20 подтверждено дизассемблером геттера |
| `IL2CPP_LIST_ITEMS` | 0x10 | List<T>._items | il2cpp.h + ABI — System_Collections_Generic_List_1_T__Fields: порядок (_items, _size, _version, _syncRoot) плюс заголовок объекта 0x10 -> 0x10 и 0x18. В дампе у generic смещений нет, поэтому считается по порядку |
| `IL2CPP_LIST_SIZE` | 0x18 | List<T>._size (int32) | il2cpp.h + ABI — тот же порядок: _size вторым -> 0x18 |
| `SYNC_VALUE_OFFSET` | 0x20 | Mirror SyncVar<T>.Value | il2cpp.h + ABI — Mirror SyncVar<T>: обёртка из двух полей, значение лежит в 0x20; найдено по содержимому (по этому смещению читается само значение SyncVar) |
| `TOD_SKY_CYCLE` | 0x40 | TOD_Sky.Cycle. Имя класса TOD_Sky обфусцировано и ротирует каждый билд (IY -> UV), поэтому не field: искать grep'ом 'TOD_CycleParameters_o* Cycle' в il2cpp.h — offset поля Cycle и есть это значение | il2cpp.h + dump.cs — TOD_Sky.Cycle: имя класса ротирует (IY -> UV). Искать класс с полем типа Cycle; проверяется «всегда день» — время суток возвращается в полдень |
| `WEAPONVIEW_INNER` | 0x10 | обёртка weapon view: внутренний объект (0x10) | il2cpp.h + dump.cs — внутренний объект обёртки (0x10) — сразу за заголовком |
| `WEAPONVIEW_PIECE` | 0x50 | обёртка weapon view: WeaponPiece | il2cpp.h + dump.cs —  WeaponPiece — второе поле того же класса, сразу за WeaponBase |
| `WEAPONVIEW_ROOT_TRANSFORM` | 0x60 | обёртка weapon view: корневой Transform модели | il2cpp.h + dump.cs — корневой Transform модели оружия в том же классе |
| `WEAPONVIEW_WEAPON_BASE` | 0x48 | обёртка weapon view (имя класса обфусцировано): указатель на WeaponBase | il2cpp.h + dump.cs — обёртка weapon view: имя класса обфусцировано и ротирует. Искать класс, у которого рядом лежат WeaponBase*, WeaponPiece* и Transform* |
| `TOD_SCAN_RVA_BEGIN` | 0xd8d0000 | НЕ поле: начало области глобальных Il2CppClass*-слотов в .data.rel.ro, которую скан в always_day_tick() перебирает в поисках TOD_Sky. Уезжает каждый билд вместе с *_TYPEINFO_RVA (было 0xD7A0000). Проверка: кандидаты typeinfo_rva.py для класса TOD_Sky обязаны попасть в [BEGIN,END) | libil2cpp.so — начало окна .data.rel.ro со слотами глобальных Il2CppClass*, которое перебирает always_day_tick(); уезжает вместе с бинарём |
| `TOD_SCAN_RVA_END` | 0xd970000 | конец окна скана TOD_Sky = BEGIN + 0xA0000 (было 0xD840000) | libil2cpp.so — конец окна скана = BEGIN + 0xA0000 |
| `IL2CPP_CLASS_NAME` | 0x10 | Il2CppClass.name (Il2CppString*) — по нему код сверяет имена классов в рантайме | libil2cpp.so (ABI, metadata v39) — Il2CppClass.name; проверяется в рантайме: код читает имя класса и сверяет со строкой — пока имена совпадают, смещение верное |
| `IL2CPP_CLASS_NAMESPACE` | 0x18 | Il2CppClass.namespace (Il2CppString*) | libil2cpp.so (ABI, metadata v39) — Il2CppClass.namespaze, следующим полем |
| `CAMERA_ASPECT` | 0x4e0 | нативная Camera: aspect, float | libunity.so — get_aspect; проверено совпадением с sw/sh экрана |
| `CAMERA_FAR_CLIP` | 0x458 | нативная Camera: far clip plane, float | libunity.so — get_farClipPlane; лежит рядом с near (+4), проверено чтением |
| `CAMERA_FOV_DEGREES` | 0x170 | нативная Camera: field of view, float (градусы) | libunity.so — get_fieldOfView; проверено поведением: круг FOV в оверлее совпадает с прицелом игры |
| `CAMERA_NATIVE_TRANSFORM` | 0x20 | нативная Camera -> её Transform | libunity.so — тот Transform, по которому Unity пересобирает кеш view при dirty-флаге; этим же указателем пользуется ESP (read_native_camera_matrices) и фрикам |
| `CAMERA_NEAR_CLIP` | 0x454 | нативная Camera: near clip plane, float — сюда пишет X-ray и отсюда же восстанавливает | libunity.so — get_nearClipPlane; проверено самой надёжной проверкой — иксрей пишет сюда, и картинка меняется, а при выключении восстанавливается |
| `CAMERA_PREV_VIEW_PROJ` | 0x5c8 | нативная Camera: матрица прошлого кадра (motion vectors) | libunity.so — матрица прошлого кадра (motion vectors); в коде не используется |
| `CAMERA_PROJECTION_MATRIX` | 0xb0 | нативная Camera (libunity.so): кеш projection Matrix4x4; ленивый, при внешнем чтении устаревает — только фолбэк | libunity.so — get_projectionMatrix_Injected @ 0x5ac53c -> хелпер 0xe1fe6c отдаёт cam+0xB0 |
| `CAMERA_PROJ_DIRTY` | 0x500 | нативная Camera: dirty-байт кеша projection (0x500) | libunity.so — dirty-байт кеша projection |
| `CAMERA_VIEW_DIRTY` | 0x502 | нативная Camera: dirty-байт кеша view (0x502) | libunity.so — dirty-байт кеша view: при движении камеры меняется, без движения — нет |
| `CAMERA_VIEW_MATRIX` | 0x70 | нативная Camera: кеш worldToCamera Matrix4x4; пересобирается только в геттерах Unity по dirty-флагу 0x502, поэтому основной путь — поза живого Transform (0x20) | libunity.so — get_worldToCameraMatrix_Injected @ 0x5ac514 -> хелпер 0xe1fe08 отдаёт cam+0x70 (записано в комментарии game_offsets.h); кеш ленивый, живёт по dirty-байту +0x502 |
| `CAMERA_WORLD_TO_CLIP` | 0xf0 | нативная Camera: кеш worldToClip Matrix4x4 | libunity.so — worldToClip идёт следом за projection; в коде не используется, только замер для лога |
| `COMPONENT_GAMEOBJECT` | 0x20 | нативный Component.m_GameObject | libunity.so — нативный Component.m_GameObject; проверено переходом компонент -> объект |
| `COMPONENT_PAIR_PTR` | 0x8 | элемент m_Component: пара {GameObject*, Component*} — указатель на компонент | libunity.so — второй указатель в паре элемента m_Component |
| `GAMEOBJECT_COMPONENT_ARRAY` | 0x20 | нативный GameObject.m_Component (массив пар) | libunity.so — нативный GameObject.m_Component: массив пар {GameObject*, Component*} |
| `GAMEOBJECT_NAME_GUESS` | 0x48 | первая догадка на GameObject.name (core::string с SSO); уточняется в рантайме перебором кандидатов по именам костей | libunity.so — core::string с SSO; уточняется в рантайме перебором кандидатов по именам |
| `TRANSFORM_CHILDREN_ARRAY` | 0x48 | нативный Transform: массив детей | libunity.so — нативный Transform: массив детей; проверено обходом иерархии (маркеры, кости, поиск трансформа камеры) |
| `TRANSFORM_CHILD_COUNT` | 0x58 | нативный Transform: число детей (int32) | libunity.so — нативный Transform: int32 число детей, проверено тем же обходом |

Дешёвая перепроверка ABI по новому `libil2cpp.so` (без запуска игры):

* `Il2CppClass.static_fields == 0xB8` — в любом методе, читающем статики:
  `adrp/ldr` слота → `ldr xA,[xM]` → `ldr xB,[xA,#0xb8]`. В билде
  `62a8534`: `OreHitstreaksMarker.Update` 0x786eb9c.
* `klass->interfaceOffsets == 0xB0`, `interface_offsets_count == 0x12E` —
  `MineableObject` 0x656781c/0x6567828 (билд `62a8534`).
* `Il2CppArray`: длина 0x18, первый элемент 0x20; `List<T>`: `_items` 0x10,
  `_size` 0x18 — см. любой перебор коллекции в коде игры.

Где что лежит, коротко: `libunity.so` — нативные Camera/Transform/
Component/GameObject (Camera ищется по геттерам `*_Injected`, этоякорь
на весь класс); `libil2cpp.so` — ABI служебных структур и слоты TypeInfo;
`il2cpp.h` — поля управляемых классов и порядок полей generic-обёрток.
Смещения generic-обёрток (`List<T>`, `Dictionary<K,V>`, `SyncVar<T>`,
`AsyncReactiveProperty<T>`) в дампе отсутствуют совсем: они считаются
порядком полей плюс заголовок объекта 0x10, а у элементов массива
структур заголовка нет — отсюда `DICT_ENTRY_STRIDE` 0x18, а не 0x28.

**Расхождение, которое надо помнить при обновлении:** `RAYCASTHIT_COLLIDER`
(0x28) в текущем дампе имеет тип `UnityEngine_EntityId_o`, а не
`UnityEngine_Collider_o*`, как было записано в карте раньше. Смещение
верное, тип в карте исправлен, но код (§ поиск «перекрытие собственным
камнем» в `game.cpp`) всё ещё сравнивает содержимое 0x28 как указатель на
коллайдер. При обновлении проверить, не стала ли эта проверка всегда
ложной — тогда руду снова начнёт «перекрывать» своим же камнем.

---

## D. Имена классов, которые сверяются в рантайме

Код читает `Il2CppClass.name` (`+0x10`) и сравнивает со строкой, поэтому имя
класса — тоже часть «оффсетов». Эти имена в текущем билде на месте:

* `MineableObjectExtension_OreHitstreaks`, `MineableObjectExtension_TreeHitstreaks`
  (цель автофарма; `kFarmExtOreClass` / `kFarmExtTreeClass` в `game.cpp`),
  а также `…_OreHitstreaksMarker`, `…_HitMarkerItem`;
* префикс `Mineable` (`MineableStone`/`MineableTree`/…), `LootObject`,
  `ItemPickup`, `LootDestroyable`, `PumpkinTrick` — маркеры ESP;
* `PlayerManager`, `GameControllerBase`, `NetworkClient`, `NetworkIdentity`,
  `List\`1` — служебные проверки.

Базовый интерфейс экстеншенов ротировал `JE`→`dk` — код его не использует
(коллекция `MINEABLE_EXTENSIONS` перебирается с определением формы на лету:
имя класса начинается с `List\`1` → список, иначе массив).

Сверка одной командой (должно быть непусто для каждого имени):

```bash
grep -c '^public class .*MineableObjectExtension_OreHitstreaks' <new>/dump.cs
```

Между `89e0b63` и `62a8534` из имён, которые знает код, исчез только `DVL` —
и это не класс в нашем смысле, а строка-лейбл в таблице `kWeaponNames`.

---

## E. Чек-лист обновления (порядок на новые дампы)

1. Распаковать новые дампы: `bash tools/offsets/extract_dumps.sh /tmp/new`
   (`bash`, не `sh` — в скрипте `set -o pipefail`).
2. Сличить отпечаток: `python3 tools/offsets/build_map.py /tmp/new --check`.
   Версия движка та же → класс C не трогаем; отличается → пересчитываем
   `libunity.so`/ABI руками (§C).
3. Сверить поля: `python3 tools/offsets/verify_map.py /tmp/new`. Выданный
   список расхождений и есть рабочий список на обновление.
4. `python3 tools/offsets/update_offsets.py --old <старый коммит> --apply` —
   сам пересчитает поля и RVA; без `--apply` он только печатает diff.
5. Имена классов (§D) сверить grep-ом по новому `dump.cs`.
6. Пересобрать карту и этот файл:
   `python3 tools/offsets/build_map.py /tmp/new` и
   `python3 tools/offsets/make_provenance.py --build <NEW> --prev <OLD>`.
7. Стенды: `sh tools/offsets/run.sh && sh tools/syntax/check.sh`.
8. Только после сборки GitHub Actions — просить проверку на устройстве.
   Первым делом проверять камеру: если PlayerManager/GameController уехал,
   сыпется вся цепочка до камеры, и это выглядит как «камера не найдена»
   и «потеря MouseLook» — симптомы, которые легко принять за баг логики.

---

## E. Функциональные якоря (для ручной перепроверки семантики)

Сам код НИ ОДНУ функцию игры не вызывает — только читает память, поэтому VA
методов не являются зависимостью. Они нужны, чтобы после апдейта убедиться,
что поле по-прежнему означает то же самое (а не просто стоит на том же
смещении). Имена методов обфусцированы и ротируют; якорь ищется по
сигнатуре (`dump.cs`: класс + типы параметров) и по характерным константам в
дизассемблерации.

| роль | билд `89e0b63` | билд `62a8534` | как узнать |
|---|---|---|---|
| `MineableObject`: ленивое заполнение `MINEABLE_EXTENSIONS` (0xE8) | `cik` 0x648e6ec | 0x65677d0 | `ldr x?,[x?,#0xe8]!` → `cbnz` → `GetComponents<dk>` → `str x0,[x22]` |
| руда: перечитывает живой маркер 0x30 и узел 0x40 каждый кадр | `Update` 0x772bfb4 | `Update` 0x786b260 | имя `Update` не обфусцировано |
| руда: запись живого маркера 0x30 | `gir`/`gil` 0x772d1ec/0x772a548 | 0x786b03c (`str x0,[x19,#0x30]`) | единственный `str` не-нуля в 0x30 |
| руда: обнуление 0x30 (крестик потух) | `giq` 0x7729c90 | 5 мест: 0x7868cc8, 0x7869a78, 0x786a56c, 0x786aed8, 0x786b600 | `str xzr,[x19,#0x30]` |
| маркер руды: таймер жизни 0x58, порог **15.0**, затем `Destroy` | `Update` 0x772f3bc | `Update` 0x786eb40 | `ldr s?,[x19,#0x58]` … `fmov s?,#15.0` |
| дерево: проверка попадания (серия засчитана) | `giL` 0x77313dc (+`rRS`/`DMU`/`Dne`/`DfW`) | `bool(Vector3)`: 0x786fa30, 0x786fbd0, 0x787331c, 0x78740e0, 0x7874280 | читают 0x50 (живой клон) и 0x88 (точка на коре) |
| дерево: пересчёт сегмента 0x88/0xA4 | `giJ`/`WO`/`Dd` | `str x8,[x19,#0x88]` в 0x7870630, 0x78712ec, 0x7872abc | парная запись 0x88 и 0xA4 |
| дерево: сброс счётчика серии 0x48 | `giD` 0x7731258 | `str wzr,[x19,#0x48]` (3 места) | — |
| `TOD_Sky` (время суток): класс с `Cycle` на 0x40 | `IY` | `UV` | `grep -n 'TOD_CycleParameters_o\* Cycle' il2cpp.h` |

Инструменты ручной сверки: `tools/offsets/il2cpp_layout.py`
(`diff`/`show`/`find`; список отслеживаемых структур — `TRACKED`, крестик там
есть) и `tools/offsets/typeinfo_rva.py`. Для дизассемблерации с аннотацией
имён полей нужен одноразовый хелпер (`capstone` + `il2cpp.h`): VA→смещение в
файле считается по PT_LOAD-сегментам ELF, а не хардкодом (в билде `89e0b63`
дельта была 0x4000, в `62a8534` — другая).

---

## F. Что вообще не из дампов

* **Таблица item-id → название оружия** (`weapon_label_for_item_id` в
  `game.cpp`): снята с устройства (`items.txt`, `/storage/emulated/0/benzhack`),
  id приходит из синхронизируемого `WeaponPiece.Number`. Из дампов не
  проверяется; при смене базы предметов — снять заново. Промах не фатален:
  неизвестный id уходит на фолбэк по имени префаба.
* **Окно скана `TOD_SCAN_RVA_BEGIN/END`**: диапазон `.data.rel.ro`, а не поле.
  Пересчёт — комментарий в `game_offsets.h`; контроль — оба кандидата
  `TOD_Sky` из `typeinfo_rva.py` обязаны попасть в окно, а число слотов в нём
  должно быть ~80 тысяч (в `89e0b63` их там 82 154, в `62a8534` — 82 167;
  35 слотов в старом окне нового билда — верный признак, что окно протухло).
* **Нативные смещения `Camera`** (§3.1 `OFFSETS_UPDATE.md`) — из `libunity.so`,
  обновляются отдельно и только при смене Unity.

## G. Поколения обфусцированных имён (чтобы не пугаться diff'а)

| билд | backing-поля | пример полей крестика |
|---|---|---|
| до `89e0b63` | `_ukT_k__BackingField` | `MoW`, `MTn`, `MTQ` |
| `89e0b63` | `_Q*_k__BackingField` | `MoW`, `MTn`, `MTQ` |
| `62a8534` | `_L*_k__BackingField` | `lzD`, `lHe`, `lHC` |

Смещения при этом не двигались — ротировали только имена. Важно: читаемые
имена полей (`hitstreakIndex`, `meshRenderer`, `lifetime`, `mark`, …) между
соседними билдами сохраняются, НО в `62a8534` впервые обфусцировалась часть
читаемых полей `PlayerWeapon`, поэтому страховка «читаемое имя не ротирует» в
`update_offsets.py` ослаблена: `_XXXX_k__BackingField` считается
обфусцированным именем и в проверке поколения не участвует.

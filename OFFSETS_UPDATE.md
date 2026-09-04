# Обновление оффсетов после апдейта игры

Этот файл — справочник для быстрого переезда на новую версию игры.
Всё, что используется для чтения памяти проекта, описано ниже вместе с тем,
где каждый оффсет был найден и как его заново локализовать после обновления.

> Единый источник правды по оффсетам в коде: **`jni/src/game_offsets.h`**.
> Все значения констант продублированы тут, чтобы не лезть в заголовок.
> После каждого обновления игры сверь этот список и поправь оба места.

---

## 1. Где что лежит

- **`jni/src/game_offsets.h`** — все оффсеты (поля инстансов, RVA TypeInfo, нативные смещения Unity).
- **`jni/src/game.cpp`** — код чтения памяти (обход объектов, строки, боксы, ESP, аим).
- **`jni/src/main.cpp`** — отрисовка ESP/меню (сами оффсеты не читает, кроме констант через game_offsets).
- **`jni/Android.mk` / `build.sh`** — сборка `libs/arm64-v8a/xvcen.sh` (GitHub Actions собирает на push).

## 2. Исходники для переопределения (после апдейта нужны свежие)

1. **`dump.cs`** — декомпилированные управляемые классы (поля с именами и порядком). Именно по нему ищем поля классов `Oxide.PlayerManager`, `FPManager`, `FPObject`, `Item`, `ItemData`, `KCC`, `Ragdoll` и т.д.
2. **`libil2cpp.so`** — глобальные `Il2CppClass*` (RVA TypeInfo) для статических полей вроде списка игроков.
3. **`libunity.so`** — нативные смещения Unity (Camera/Transform/GameObject).

Способы получить: дамп памяти (GameGuardian/после выбора процесса), `il2cppdumper` по `libil2cpp.so` + `global-metadata.dat`, распаковка APK.

---

## 3. Полный список оффсетов (текущие значения)

### 3.1. Unity Camera — нативные смещения (из libunity.so)
| Константа | Смещение | Откуда / комментарий |
|---|---|---|
| `CAMERA_NATIVE_TRANSFORM` | 0x20 | `Transform*`, живой источник для rebuild view (исп. чтобы обойти "дрейф ESP") |
| `CAMERA_PROJECTION_MATRIX` | 0xB0 | lazy-кеш, getter не вызывается извне → устаревает |
| `CAMERA_VIEW_MATRIX` | 0x70 | lazy-кеш worldToCamera → **устаревает**, не использовать напрямую |
| `CAMERA_WORLD_TO_CLIP` | 0xF0 | projection × worldToCamera |
| `CAMERA_PREV_VIEW_PROJ` | 0x5C8 | previousViewProjection |
| `CAMERA_FOV_DEGREES` | 0x170 | get_fieldOfView storage |
| `CAMERA_ASPECT` | 0x4E0 | |
| `CAMERA_NEAR_CLIP` | 0x454 | |
| `CAMERA_FAR_CLIP` | 0x458 | |
| `CAMERA_VIEW_DIRTY` | 0x502 | byte dirty-флаг w2c |
| `CAMERA_PROJ_DIRTY` | 0x500 | byte dirty-флаг proj |

### 3.2. Il2Cpp / универсальные layout
| Константа | Значение |
|---|---|
| `MANAGED_CACHED_PTR` | 0x10 |
| `IL2CPP_STRING_LENGTH` | 0x10 (int32) |
| `IL2CPP_STRING_CHARS` | 0x14 (UTF-16) |
| `IL2CPP_ARRAY_LENGTH` | 0x18 |
| `IL2CPP_LIST_ITEMS` | 0x10 |
| `IL2CPP_LIST_SIZE` | 0x18 |
| `IL2CPP_ARRAY_FIRST_ELEMENT` | 0x20 |

> Layout System.String / List / Array практически не меняется между версиями.
> Проверяются по dump.cs только если что-то сломалось.

### 3.3. Глобальные TypeInfo RVA (libil2cpp.so)
| Константа | RVA | Класс |
|---|---|---|
| `PLAYER_MANAGER_TYPEINFO_RVA` | 0xD48CFB0 | Oxide.PlayerManager |
| `GAME_CONTROLLER_TYPEINFO_RVA` | 0xD4884E8 | GameControllerBase |
| `PLAYER_MANAGER_STATIC_FIELDS_LIST` | 0x10 | clientPlayerList (поле статики) |
| `GAME_CONTROLLER_LOCAL_PLAYER_FIELD` | 0x10 | `<LZp>k__BackingField` (локальный игрок) |
| `GAME_CONTROLLER_CAMERA_MANAGER_FIELD` | 0x38 | `<LZD>k__BackingField` |
| `CAMERA_MANAGER_CAMERA_FIELD` | 0x20 | m_Camera |

### 3.4. Oxide.PlayerManager — поля инстанса (по dump.cs)
| Константа | Смещение | Поле в dump.cs |
|---|---|---|
| `PLAYER_TRANSFORM` | 0x68 | worldCameraRoot |
| `PLAYER_POSITION` | 0x1D0 | lastSavedPosition (0x1C8 = lastTickPosition) |
| `PLAYER_CHARACTER_MODEL` | 0x150 | characterModel (GameObject) |
| `PLAYER_NICKLABEL` | 0x130 | nicklabel (wK) |
| `PLAYER_DISPLAY_NAME` | 0x220 | **LLI — реальное человеческое имя** (см. §4) |
| `PLAYER_EVENT_HANDLER` | 0x78 | playerEventHandler (fvp) |
| `PLAYER_FP_MANAGER` | 0x90 | fpManager |
| `PLAYER_KCC_REFERENCE` | 0xB0 | kccReference |
| `PLAYER_VOICE_PLAYER` | 0x140 | voicePlayer (fuI tracker) |
| `PLAYER_VOICE_STATE` | 0x2E8 | LLT (VoicePlayerState) |

Полезные строковые поля PlayerManager (сверялись дампом):
userID≈0x278, teamName≈0x280, clanId≈0x290, clanTag≈0x298, observedId≈0x320.
**userID/voice — это машинные коды, НЕ имя.** Имя — только `LLI@0x220`.

### 3.5. Nicklabel wK + UI.Text
| Константа | Смещение |
|---|---|
| `NICKLABEL_PLAYER_BACKREF` | 0x20 |
| `NICKLABEL_NICKNAME_TEXT` | 0x38 |
| `UI_TEXT_MTEXT` | 0xE0 (UnityEngine.UI.Text.m_Text) |
> Приватные строки виджета: ~0xA8, 0xB0, 0xB8 (zvy@B0 = то же имя, что LLI@0x220).

### 3.6. FP / оружие / предметы
| Константа | Смещение | Поле/тип |
|---|---|---|
| `FPMANAGER_CURRENT_WEAPON` | 0x58 | LtZ (current FPWeaponBase) |
| `FPMANAGER_CURRENT_OBJECT` | 0x50 | _currentWeapon (FPObject) |
| `FPMANAGER_AIM_BLEND` | 0xA8 | `<LtX>k__BackingField` |
| `FPOBJECT_OBJECT_NAME` | 0x78 | имя FP-объекта |
| `FPOBJECT_PLAYER_BACKREF` | 0xC0 | Player |
| `FPOBJECT_ITEM` | 0x40 | `<Ltl>k__BackingField` (Item) |
| `ITEM_DATA` | 0x20 | `<LIN>k__BackingField` (ItemData) |
| `ITEMDATA_NAME` | 0x18 | m_Name |
| `ITEMDATA_SHORTNAME` | 0x20 | m_ShortName |
| `FPWEAPON_IS_AIMING` | 0x120 | `<LKk>k__BackingField` |
> Внимание: FP-объекты у удалённых игроков часто **null** (не синхронизированы).
> `player_weapon_name()` проходит строгий (back-ref==player) и ослабленный проход.

### 3.6.1. Оружие удалённых игроков (третье лицо, синхронизируется)
FP-цепочка выше живёт только у локального игрока, поэтому имя оружия врагов
берётся из сетевого компонента `HyperHug.Games.Oxide.Features.Weapons.PlayerWeapon`
(`Mirror.NetworkBehaviour`, dump.cs) — он есть у каждого игрока.

| Константа | Смещение | Поле/тип |
|---|---|---|
| `PLAYERWEAPON_VIEW` | 0x90 | `playerWeaponViewReference` (`Ms` → класс `Mo`) |
| `PLAYERWEAPON_PIECE` | 0xD8 | `Oxide.WeaponPiece` (SyncVar weaponPiece, 0x10 байт) |
| `PLAYERWEAPON_STATE` | 0xE8 | `WeaponState` |
| `PLAYERWEAPON_PLAYER_BACKREF` | 0x100 | `<player>k__BackingField` — валидация кандидата |
| `WEAPONPIECE_ENABLED` | +0x00 | bool Enabled |
| `WEAPONPIECE_NUMBER` | +0x02 | short Number (id предмета) |
| `WEAPONVIEW_WEAPON_BASE` | 0x48 | `Mo.Zjj` → `WeaponBase` (MonoBehaviour на префабе оружия) |
| `WEAPONVIEW_PIECE` | 0x50 | `Mo.Zjk` → WeaponPiece |
| `WEAPONVIEW_ROOT_TRANSFORM` | 0x60 | `Mo.Zjo` → Transform префаба |
| `WEAPONVIEW_INNER` | 0x10 | `fSN.IDA` — декоратор поверх другого `Ms` |
| `MODELINFO_RIGHT_WEAPON_HOLDER` | 0x28 | `PlayerModelInfo.rightWeaponHolder` |
| `MODELINFO_LEFT_WEAPON_HOLDER` | 0x30 | `PlayerModelInfo.leftWeaponHolder` |
| `CHARANIM_PLAYER_MODEL_INFO` | 0x30 | `CharacterAnimation.playerModelInfo` |
| `INVDATA_PLAYER_MODEL_INFO` | 0x20 | `PlayerInventoryData.playerModelInfo` |
| `IL2CPP_CLASS_NAME` | 0x10 | `Il2CppClass.name` (проверка класса `PlayerWeapon`) |

Как это читается (`remote_weapon_display_name()` в game.cpp):
1. `PlayerManager.weaponReference (0xF0)` — обфусцированная обёртка (как
   `kccReference`): проверяем сам указатель, затем поля 0x08..0x60, затем
   сканируем поля `PlayerManager` 0x68..0x350. Кандидат принимается только
   если `+0x100 == player` **и** имя класса == `PlayerWeapon`.
2. Маршрут A: `PlayerWeapon → view(0x90) → WeaponBase(0x48)` → нативный
   GameObject → его имя = имя префаба оружия.
3. Маршрут B (независимый): `PlayerModelInfo.rightWeaponHolder/left` → первый
   ребёнок (или внук) → имя GameObject. `PlayerModelInfo` берётся из
   `inventory(0x98) → data(0x20) → 0x20` либо `KCC → CharacterAnimation(0x108) → 0x30`.
4. Если ничего не разрешилось, но SyncVar говорит, что оружие в руках —
   показывается `WPN <Number>` (значит, сломался только шаг с именем префаба).
5. Имя чистится (`weapon_label_from_object_name`): срезается `(Clone)`,
   индекс и маркер скина (`07_Default…`, `07_Skin…`, `12_Skin2…`),
   префиксы `tp_/fp_/w_/...`, суффиксы `_tp/_view/_model/...`, отбрасываются
   служебные узлы (`WeaponHolder`, `Root`, `Default`, ...).
6. Опечатка префабов правится (`fix_weapon_label_spelling`): `Riffle` → `Rifle`,
   чтобы у своего игрока (имя из `ItemData.m_Name`) и у чужих (имя префаба)
   одно и то же оружие подписывалось одинаково.
7. По умолчанию подпись **на русском** (`kWeaponLabelRussian = true`);
   поставь `false` — будут обычные названия игры (`Assault Rifle`).
   Таблица `kWeaponNames` (`key` / `en` / `ru`) — не белый список, а
   канонизатор: она приводит подпись к одному виду независимо от источника
   (имя префаба, `ItemData.m_Name`, shortname) и от лишних слов, которые может
   принести префаб скина (`07_SkinCamoAssault Riffle` → `Assault Rifle`).
   Оружие, которого в таблице нет, **всё равно отображается** — очищенным
   именем префаба, так что новые стволы после апдейта игры не пропадут.
   Ключи нормализуются (только буквы и цифры, нижний регистр), сравнение
   сначала точное, потом по самой длинной подстроке — поэтому `pickaxehammer`
   не путается с `pickaxe`, а `crossbow` с `bow`.
   Список предметов игры для сверки таблицы — класс `rs` в `dump.cs`
   (все shortname'ы: `assault.rifle`, `thompson`, `dvl`, `kriss.vector`, …),
   типы оружия — enum `WeaponType` (Pistol, SMG, Shotgun, AR, DMR, Sniper,
   LMG, Launcher, Bow, Crossbow, Grenade, Explosive, Melee, Spear, Tool).
   Буферы подписи — 48 байт (`EspBox::weapon`), т.к. кириллица в UTF-8 шире.

Оффсет имени GameObject подбирается в рантайме (`ensure_gameobject_name_offset`),
поэтому подписи оружия работают и при выключенном скелете.

### 3.6.2. ESP руд и животных (Mirror-реестр)

Руды, деревья и животные — всё это наследники `Oxide.MineableObject`
(`MineableStone` / `MineableTree` / `MineableAnimal` / `MineableObjectWithRandomSpawn`),
и каждый — `Mirror.NetworkBehaviour`. Поэтому список берётся прямо из клиента Mirror:

```
NetworkClient.spawned (Dictionary<uint, NetworkIdentity>)
  -> NetworkIdentity.NetworkBehaviours[]  -> компонент Mineable* -> entityType
  -> GameObject этого identity           -> нативный Transform -> позиция в мире
```

| Константа | Значение | Что это |
|---|---|---|
| `NETWORK_CLIENT_TYPEINFO_RVA` | 0xD48C270 | слот `Il2CppClass*` для `Mirror.NetworkClient` в `.data` |
| `NETWORK_CLIENT_SPAWNED` | 0x28 | `spawned` в статике класса (`klass+0xB8`) |
| `DICT_ENTRIES` / `DICT_COUNT` | 0x18 / 0x20 | поля `Dictionary` |
| `DICT_ENTRY_STRIDE` / `DICT_ENTRY_VALUE` | 0x18 / 0x10 | `Entry {int hash; int next; uint key; obj value}` |
| `NETID_BEHAVIOURS` | 0x80 | `NetworkIdentity.NetworkBehaviours[]` |
| `MINEABLE_ENTITY_TYPE` | 0xD8 | `EntityType` (см. enum `MineableEntityType`) |
| `MINEABLE_FRACTION` / `MINEABLE_MAX_HEALTH` | 0xD0 / 0xC0 | остаток / максимум прочности |
| `GAME_CONTROLLER_NET_IDENTITY_FIELD` | 0x8 | эталонный `NetworkIdentity` для проверки класса |

**Как заново найти `NETWORK_CLIENT_TYPEINFO_RVA` после апдейта.** В `dump.cs` у
класса `Mirror.NetworkClient` нет TypeInfo-RVA, его берут из `libil2cpp.so`:

1. Распаковать `libil2cpp.7z` (в архиве фильтр ARM64, `py7zr` его не умеет):
   `xz -d --format=raw --arm64 --lzma2=dict=32MiB -c packed.bin > libil2cpp.so`
   (сырой поток — это файл 7z с 32-го байта, размер = `packsizes[0]`).
2. Взять адрес любого метода `Mirror.NetworkClient$$...` из `script.json` и
   дизассемблировать (capstone, `offset = RVA - 0x4000`). Виден шаблон:
   `adrp x19,#0xd165000 ; ldr x19,[x19,#0x530]` → это запись GOT.
3. В `.rela.dyn` найти запись с `r_offset == 0xd165530`; её addend
   (`R_AARCH64_RELATIVE`, 0x403) и есть искомый RVA слота.
4. Проверка там же в коде: `ldr x0,[x19]` → `ldr x8,[x0,#0xb8]` (static_fields)
   → `ldr x0,[x8,#0x28]` (`spawned`).

Классы компонентов не хардкодятся: у каждого `NetworkBehaviour` в рантайме
читается имя класса (`Il2CppClass.name`), и всё, что начинается на `Mineable`,
считается добываемым объектом (результат кешируется по указателю класса).
Тип объекта берётся из `entityType`; на экран выводятся только руды
(Stone/Iron/Sulfur/Ice) и животные (Bear/Boar/Deer/Rabbit/Hare/Chicken/Fish/Cannibal),
деревья и бочки намеренно отфильтрованы. Реестр пересканируется раз в ~3 с,
позиции руд — только при пересканировании (они статичны), у животных — каждый кадр.
Диагностика первого скана: `/storage/emulated/0/Download/xvcen_marker_debug.log`.

### 3.7. Aим / события игрока (ADS)
| Константа | Смещение |
|---|---|
| `EVENT_HANDLER_MANAGER_BACKREF` | 0xD0 |
| `EVENT_HANDLER_AIM_ACTIVITY` | 0x268 |
| `EVENT_HANDLER_LOOK_DIRECTION` | 0x140 |
| `SYNC_VALUE_OFFSET` | 0x20 |
| `ACTIVITY_ACTIVE_FLAG` | 0x10 |

### 3.8. KCC / Ragdoll / скелет
| Константа | Смещение |
|---|---|
| `KCC_PLAYER_BACKREF` | 0x78 |
| `KCC_HEAD_TRANSFORM` | 0x88 |
| `KCC_NORMAL_HEIGHT` | 0xA0 |
| `KCC_CROUCH_HEIGHT` | 0xA4 |
| `KCC_HITBOX_ROOT` | 0x70 |
| `KCC_CHARACTER_ANIMATION` | 0x108 |
| `KCC_LOOK_HEIGHT_OFFSET` | 0x90 |
| `KCC_MOVE` | 0x16C (структура Move, см. заголовок) |
| `HITBOX_ROOT_ARRAY` | 0x68 |
| `HITBOX_SIZE` | 0x24 |
| `HITBOX_CENTER` | 0x30 |
| `HITBOX_AREA` | 0x68 (0 голова, 1 грудь, 2 нога, 3 стопа, 4 рука) |
| `CHAR_ANIM_PLAYER_BACKREF` | 0x78 |
| `CHAR_ANIM_RAGDOLL` | 0x38 |
| `RAGDOLL_PELVIS_RIGIDBODY` | 0x20 |
| `RAGDOLL_BONES_ARRAY` | 0x88 |
| `RAGDOLL_BODYPART_TRANSFORM` | 0x10 |

### 3.9. Нативные Unity (Transform / GameObject / Component)
| Константа | Смещение | Откуда |
|---|---|---|
| `TRANSFORM_CHILDREN_ARRAY` | 0x48 | ldr x8,[x0,#0x48] |
| `TRANSFORM_CHILD_COUNT` | 0x58 | ldr w0,[x0,#0x58] |
| `COMPONENT_GAMEOBJECT` | 0x20 | ldr x0,[x0,#0x20] |
| `GAMEOBJECT_COMPONENT_ARRAY` | 0x20 | |
| `COMPONENT_PAIR_PTR` | 0x08 | |
| `GAMEOBJECT_NAME_GUESS` | 0x48 | (проверяется по именам костей) |

---

## 4. Ключевые правила (не забудь после апдейта)

1. **Имя игрока (ник) читается ТОЛЬКО из `PlayerManager + 0x220` (`LLI`).**
   Это подтверждено в рантайме: `LLI@0x220` == приватная строка никлейбла
   (`пахановский`, `#Фришка`, `dusterhuffer`, ...). Голосовые поля
   (`PLAYER_VOICE_STATE.NAME`, `voicePlayer.tag`) и `userID` дают только
   машинный код `932D3ABF57D64819` — **не используй их как имя**, максимум фолбэк.
2. `CAMERA_VIEW_MATRIX@0x70` / `PROJECTION@0xB0` — ленивые кеши; при внешнем
   чтении устаревают → ESP "плывёт". Для камеры перестраивай вью от живого
   `Transform@0x20`.
3. Пути к объектам старайся **валидировать back-ref** (равенство указателей
   обратно на игрока), чтобы не читать чужие/мёртвые объекты.

---

## 5. Процедура обновления после патча игры

1. Достань свежие `dump.cs`, `libil2cpp.so`, `libunity.so`, `global-metadata.dat`.
2. Открой `dump.cs` и для каждого класса, который мы читаем, сверь **порядок и
   смещения полей** (в dump.cs смещение поля = сумма размеров предыдущих +
   смещение базового класса, обычно видно в шестнадцатеричном комменте или
   высчитывается порядком следования). Помни: у инстанс-полей есть
   `Object header`/`klass/monitor` и поля базового класса.
3. Сверь константы из §3 по списку. Если поле "уехало", обнови **и**
   `game_offsets.h`, и эту таблицу.
4. RVA `TypeInfo` (`..._TYPEINFO_RVA`) — это адреса глобальных в libil2cpp;
   ищи по имени класса (символы вида `_ZN...Il2CppClass...`/по таблице GOT)
   или через `il2cppdumper` (там бывает `[RVA] Token`).
5. Нативные Unity-смещения не меняются между версиями Unity-рантайма — только
   если игра обновила сам Unity.
6. Пересобери: пушишь в `arena/01a068cd-myproject1` → GitHub Actions соберёт
   `xvcen-sh-arm64-v8a` → забираешь артефакт из CI.
7. Проверка на устройстве: ESP показывает **реальные ники**, корректное оружие,
   боксы не сливаются. Если что-то не так — включи отладку/верни временный дамп
   полей (см. историю коммитов: функция `dump_player_diagnostics`, удалённая в
   `e1eb40c`, писала в `/storage/emulated/0/Download/xvcen_esp_debug.log`).

## 6. Как вернуть временную диагностику (если снова что-то сломалось)
Смотри коммит `08b5b6f` — там была функция `dump_player_diagnostics()` + вызов в
`esp_get_boxes()` + поле `diag_done` в `PlayerTextCache`. Её убрали в `e1eb40c`.
При регрессе проще всего восстановить её из `git show 08b5b6f` и снова
посмотреть, какие поля чем заполнены на новом билде.

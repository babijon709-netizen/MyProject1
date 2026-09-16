# Обновление оффсетов после апдейта игры

Этот файл — справочник для быстрого переезда на новую версию игры.
Всё, что используется для чтения памяти проекта, описано ниже вместе с тем,
где каждый оффсет был найден и как его заново локализовать после обновления.

> Единый источник правды по оффсетам в коде: **`jni/src/game_offsets.h`**.
> Все значения констант продублированы тут, чтобы не лезть в заголовок.
> После каждого обновления игры сверь этот список и поправь оба места.

---

## 1. Где что лежит

- **`jni/src/game_offsets.h`** — все оффсеты (поля инстансов, RVA TypeInfo, нативные смещения Unity). Это набор РЕЛИЗА.
- **`jni/src/game_offsets_beta.h`** — тот же список для БЕТЫ; собирается `tools/offsets/beta_offsets.py` из `dump_beta.7z`, руками не правится.
- **`jni/src/game_offsets_active.h` + `game_offsets_active.inc`** — активный набор: те же имена констант переменными и переключатель `go::SelectBuild()` (релиз/бета). Выбор хранится в файле `.build` рядом с конфигами.
- **`jni/src/game.cpp`** — код чтения памяти (обход объектов, строки, боксы, ESP, аим).
- **`jni/src/main.cpp`** — отрисовка ESP/меню (сами оффсеты не читает, кроме констант через game_offsets).
- **`jni/Android.mk` / `build.sh`** — сборка `libs/arm64-v8a/xvcen.sh` (GitHub Actions собирает на push).

## 2. Исходники для переопределения (после апдейта нужны свежие)

0. **`il2cpp.h`** (внутри `dump.7z`) — **главный источник смещений**: у каждого
   поля стоит комментарий `/* 0xNN */`. Именно по нему сверяются `Oxide.PlayerManager`,
   `FPManager`, `FPObject`, `Item`, `ItemData`, `KCC`, `Ragdoll` и остальные.
1. **`dump.cs`** — управляемые классы без смещений; полезен значениями enum'ов
   (`MineableEntityType`) и сигнатурами.
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
| `PLAYER_MANAGER_TYPEINFO_RVA` | 0xD8DB8B8 | Oxide.PlayerManager |
| `GAME_CONTROLLER_TYPEINFO_RVA` | 0xD8D61E8 | GameControllerBase |
| `NETWORK_CLIENT_TYPEINFO_RVA` | 0xD8DAB08 | Mirror.NetworkClient |
| `TOD_SCAN_RVA_BEGIN..END` | 0xD8D0000..0xD970000 | окно скана TOD_Sky (не поле!) |

> Значения — для билда `62a8534`. Источник истины — `jni/src/game_offsets.h`;
> таблица здесь может отстать, заголовок не может.
| `PLAYER_MANAGER_STATIC_FIELDS_LIST` | 0x10 | clientPlayerList (поле статики) |
| `GAME_CONTROLLER_LOCAL_PLAYER_FIELD` | 0x10 | `<ukT>k__BackingField` (локальный игрок) |
| `GAME_CONTROLLER_CAMERA_MANAGER_FIELD` | 0x38 | `<ukA>k__BackingField` |
| `CAMERA_MANAGER_CAMERA_FIELD` | 0x20 | m_Camera |

### 3.4. Oxide.PlayerManager — поля инстанса (по dump.cs)
| Константа | Смещение | Поле в dump.cs |
|---|---|---|
| `PLAYER_TRANSFORM` | 0x68 | worldCameraRoot |
| `PLAYER_POSITION` | 0x1D0 | lastSavedPosition (0x1C8 = lastTickPosition) |
| `PLAYER_CHARACTER_MODEL` | 0x150 | characterModel (GameObject) |
| `PLAYER_NICKLABEL` | 0x130 | nicklabel (класс `ij`, ранее `wK`) |
| `PLAYER_DISPLAY_NAME` | 0x220 | **реальное человеческое имя** (поле `uWc`, ранее `LLI`; см. §4) |
| `PLAYER_EVENT_HANDLER` | 0x78 | playerEventHandler (класс `pmi`, ранее `fvp`) |
| `PLAYER_FP_MANAGER` | 0x90 | fpManager |
| `PLAYER_KCC_REFERENCE` | 0xB0 | kccReference |
| `PLAYER_VOICE_PLAYER` | 0x140 | voicePlayer (класс `pJk`, ранее `fuI`) |
| `PLAYER_VOICE_STATE` | 0x2E8 | VoicePlayerState (поле `uWr`, ранее `LLT`) |
| `PLAYER_USER_ID` | 0x278 | `string userID` (уникален на аккаунт) |
| `PLAYER_VEHICLE_ID` | 0x288 | `uint vehicleID` (SyncVar, 0 = не в транспорте) |
| `PLAYER_SEAT_ID` | 0x28C | `uint seatID` (SyncVar) |

Полезные строковые поля PlayerManager (сверялись дампом):
userID≈0x278, teamName≈0x280, clanId≈0x290, clanTag≈0x298, observedId≈0x320.
**userID/voice — это машинные коды, НЕ имя.** Имя — только поле по `0x220`.

### 3.5. Nicklabel (`ij`, ранее `wK`) + UI.Text
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
| `PLAYERWEAPON_VIEW` | 0xD0 | `playerWeaponViewReference` (класс `sR`, ранее `Mo`) |
| `PLAYERWEAPON_PIECE` | 0x100 | `Oxide.WeaponPiece` (SyncVar weaponPiece, 0x10 байт) |
| `PLAYERWEAPON_STATE` | 0x110 | `WeaponState` |
| `PLAYERWEAPON_PLAYER_BACKREF` | 0x128 | `<player>k__BackingField` — валидация кандидата |

> ⚠️ Апдейт игры вставил **0x40 байт новых полей перед `animator`**, поэтому
> вся четвёрка уехала (было 0x90 / 0xD8 / 0xE8 / 0x100). В новой раскладке
> появился **второй** `WeaponPiece` по 0xA8 — это НЕ SyncVar, брать нельзя.
| `WEAPONPIECE_ENABLED` | +0x00 | bool Enabled |
| `WEAPONPIECE_NUMBER` | +0x02 | short Number (id предмета) |
| `WEAPONVIEW_WEAPON_BASE` | 0x48 | `sR` → `WeaponBase` (MonoBehaviour на префабе оружия) |
| `WEAPONVIEW_PIECE` | 0x50 | `sR` → WeaponPiece |
| `WEAPONVIEW_ROOT_TRANSFORM` | 0x60 | `sR` → Transform префаба |
| `WEAPONVIEW_INNER` | 0x10 | декоратор (класс `pGW`, ранее `fSN`) поверх другого view |
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
| `NETWORK_CLIENT_TYPEINFO_RVA` | 0xD7E35B8 | слот `Il2CppClass*` для `Mirror.NetworkClient` в `.data` |
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

**Определение типа — три источника, по порядку.**

1. `entityType` (`ServerPlayersAnalytics.EntityType`) — заполнен у медведей,
   кабанов, оленей, кроликов, зайцев, кур, рыбы и каннибалов.
2. **Имя префаба GameObject** — волков и крыс в enum'е нет вообще, поэтому
   читается имя GameObject компонента (а если не совпало — имя GameObject
   самого identity) и разбивается на слова: по разделителям, цифрам и
   camelCase-«горбам» (`NPC_Wolf 02(Clone)` → `npc`,`wolf`,`clone`;
   `SewerRat` → `sewer`,`rat`; `WOLF` → `wolf`). Сравнение идёт по **целым
   словам**, поэтому `Crate`, `Ratchet` или `Pirate` крысой не становятся.
   Таблица: wolf → Волк, rat/mouse → Крыса, плюс bear/boar/deer/rabbit/hare/
   chicken/fish/shark/cannibal/horse/goat/sheep/cow/fox/snake.
3. **Лут узла** — у рудных узлов `entityType` пустой, поэтому читается
   `MineableObject.m_Loot` (0xA0, `List<Oxide.LootItem>`) → `LootItem.ItemName`
   (0x10, строка shortname). Совпадения: `sulfur*` → Сера, `metal*` → Железо,
   `stone` → Камень; `wood`/`cloth`/`raw.meat` не дают ничего, поэтому деревья,
   кусты и трупы отсеиваются сами. Ранги нужны из-за того, что серные и
   железные узлы дают ещё и камень: побеждает более ценный ресурс.

Лёд (`EntityType::Ice`, лут `ice*`) намеренно **не рисуется**.

**Один identity = несколько маркеров.** Mirror собирает компоненты через
`GetComponentsInChildren`, поэтому на одном `NetworkIdentity` висит целый
кластер камней. Маркер строится на каждый `Mineable*`-компонент, и позиция
берётся с его собственного GameObject, а не с корня identity. Близкие пилюли с
одинаковой подписью прореживаются (30 px), остаётся ближайшая.

Цвета руд фиксированные (выбор цвета у строки «Руды» убран): камень — серый,
железо — оранжевый, сера — жёлтая. Цвет животных настраивается.
Подпись маркера рисуется уменьшенным шрифтом (0.78 от размера подписей игроков),
под ней второй пилюлей — дистанция (`%.0fm`) цветом «Дистанция».
Деревья отфильтрованы. Реестр пересканируется раз в ~3 с,
позиции руд и ящиков — только при пересканировании (они статичны), у животных — каждый кадр.
Дальность отрисовки маркеров задаётся слайдером «Дальность маркеров» (25…300 м,
по умолчанию 150) и приходит в игровой слой через `esp_set_marker_max_distance()`.

### 3.6.3.0. Бочки — это НЕ `LootObject`

Бочку не открывают, её разбивают, поэтому в игре она `Oxide.MineableObject`
(как руда и деревья) с `entityType = Barrel (13)`. Через код `LootObject` она не
проходит вообще, сколько бы слов `barrel` ни было в таблице подписей — из-за
этого бочки не появлялись. Разбор `entityType` (`ServerPlayersAnalytics.EntityType`):

| Значение | Что это | Что рисуем |
|---|---|---|
| 8 `Tree`, 15 `RoadSign`, 16 `StackOfWood`, 12 `Ice` | дерево / знак / брёвна / лёд | ничего |
| 9 `Stone`, 10 `Iron`, 11 `Sulfur` | руда | Камень / Железо / Сера |
| **13 `Barrel`** | бочка | **Скрап** (в категории «Лут») |
| **14 `Lootbox`** | разбиваемый ящик | **Ящик** (в категории «Лут») |
| 17 `Construction`, 18 `Deployable` | постройки игроков | ничего |
| 1–7, 22 | животные | по названию |

Дополнительно: если `entityType` пуст, бочка ловится по слову `barrel` в имени
префаба (как волки), а из «рудного» разбора лута убраны обработанные предметы
(`frag`, `pipe`, `sheet`, `scrap`, `spring`, `gear`) — из-за `metal.fragments`
внутри бочки она раньше могла определиться как железная руда.

### 3.6.3. Лутовые ящики (`Oxide.LootObject`)

Всё, что открывается, — это `Oxide.LootObject : fNZ : Mirror.NetworkBehaviour`
(`dump.cs` строка ~202468). Тот же класс используют **ящики, поставленные
игроками**, поэтому их надо отсеять. Смещения:

| Константа | Смещение | Поле в dump.cs |
|---|---|---|
| `LOOTOBJECT_INVENTORY` | 0xA0 | `Oxide.Inventory inventory` |
| `LOOTOBJECT_IS_LOOTABLE` | 0xA8 | `bool isLootable` |
| `LOOTOBJECT_PANEL_NAME` | 0xE0 | `string panelName` |
| `LOOTOBJECT_BUILDING_PIECE` | 0xF8 | `Building.BuildingPiece m_Piece` (было 0xF0) |

**Как отсеиваются ящики игроков — два независимых признака:**

1. `m_Piece != null`. Всё, что игрок ставит, — это building piece; у мировых
   ящиков это поле пустое. Главный фильтр.
2. Имя префаба. Слово `box` вместе со словом размера/материала
   (`small`, `large`, `big`, `wood`, `wooden`, `medium`, `mini`) = ящик игрока:
   так отсеиваются «большой ящик» и «маленький ящик». Плюс чёрный список
   развёрнутых объектов: `storage`, `stash`, `cupboard`, `furnace`, `campfire`,
   `locker`, `bed`, `sleeping`, `shelf`, `planter`, `composter`, `fridge`,
   `mailbox`, `workbench`, `quarry`, `turret`, `smelter`, `barbecue`, `oven`,
   `wardrobe`, `rack`.
   **В этот список нельзя класть слово, которое может встретиться у мирового
   контейнера.** `generic` пробыл там одну сборку и спрятал все бочки: бочки
   открывают обычную панель лута `generic`.
3. Спящие игроки и мешок, который остаётся после смерти игрока, — тоже
   `LootObject`. Для них отдельный чёрный список: `corpse`, `ragdoll`,
   `sleeper`, `player`, `human`, `survivor`, `backpack`, `deathbag`, `death`,
   `grave`, `skeleton`, `lootbag`, `dropbag`, `inventory`, `belt`.

**Три источника имени:** GameObject компонента, GameObject корневого
`NetworkIdentity` и `panelName` (id панели лута). Последний часто единственный,
кто прямо говорит `militarycrate`. `panelName` — одно слитное слово, поэтому в
чёрном списке лежат и слитные написания (`largewoodbox`, `woodbox`, `smallbox`,
`toolcupboard`, …).

Подпись берётся из таблицы по словам имени, ранг 4 у ценного лута перебивает
ранг 2 у обычного (иначе `MilitaryCrate` стал бы просто «Ящиком»):

| Ранг | Слова | Подпись |
|---|---|---|
| 4 | `military`, `militarycrate`, `milcrate`, `mil`, `army`, `soldier` | Военный ящик |
| 4 | `elite`, `elitecrate`, `eliteloot`, `epic`, `legendary` | Элитный ящик |
| 4 | `rare` | Редкий ящик |
| 4 | `airdrop`, `supply` | Аирдроп |
| 4 | `medical`, `ammo`, `toolbox`, `food`, `heli`, `oilrig`, `hackable`, `safe`, `cash`, `vending` | по смыслу |
| 2 | `barrel` | **Скрап** (бочки — источник скрапа) |
| 2 | `crate`, `lootbox`, `loot`, `container`, `chest`, `case`, `cache`, `trash`, `garbage` | Ящик / Контейнер / … |

Элитные ящики (ранг 4, слова `elite*`, `epic`, `legendary`) помечаются флагом
`rainbow`: он идёт `MarkerLook` → `MarkerEntity` → `EspMarker.rainbow`, и в
`DrawEspOverlay()` их подпись рисуется переливающимся цветом (полный оборот по
спектру за 2 секунды, `ImGui::ColorConvertHSVtoRGB`).

**Если ни одно слово не совпало — объект не рисуется.** Раньше он подписывался
общим «Ящик», и именно из-за этого на экране появлялись ящики игроков в домах,
спящие игроки и мешки с трупов: у них в имени нет ни одного «мирового» слова.
Мировые контейнеры всегда называют себя сами. Обратная сторона: если у какого-то
настоящего мирового ящика префаб назван никак, он пропадёт — тогда нужно узнать
его имя и добавить слово в таблицу.

Слова `bag` / `sack` из таблицы убраны намеренно: под них попадали именно мешки
после смерти игрока.

### 3.6.6. Восстановление после смерти / перезагрузки карты

Симптом был такой: после смерти игрока (иногда с респавном в другом месте и
перезагрузкой карты) пропадали **все** метки до перезапуска приложения.
Причина — три проверки, требовавшие **минимум двух игроков** в списке:

| Функция | Проверка | Что ломалось |
|---|---|---|
| `evaluate_player_position_offset` | `valid < 2` → offset невалиден | в одиночку оффсет позиции больше не подтверждался никогда |
| `optimize_matrix_configuration` | `samples.size() < 2` → сбрасывала `g_player_position_validated` | вечный цикл: сброс → повторный поиск → снова сброс |
| `discover_transform_hierarchy_layout` | `< 2` transform'ов | запасной путь тоже не мог включиться |

Если на сервере ты один, любой сброс валидации (а смерть/респавн его вызывает)
приводил к тому, что `esp_get_boxes()` каждый кадр выходил на первом же `return`
и не публиковал `g_frame_vp_valid` / `g_frame_local_valid`, без которых
`esp_get_markers()` тоже сразу выходит. Отсюда «пропало всё».

Что сделано:

* одиночная выборка принимается, если позиция похожа на мировую
  (`position_looks_like_world_space`: |x|,|z| ≤ 20000, |y| ≤ 10000, не нули);
  разброс между игроками проверяется только когда игроков ≥ 2;
* `optimize_matrix_configuration` больше не сбрасывает валидацию из-за малой
  выборки — при пустой выборке просто повторяет на следующем кадре;
* определение перезагрузки мира срабатывает уже при **одном** игроке в старом
  списке (раньше требовалось ≥ 2, поэтому соло-респавн не сбрасывал кэши);
* сторож `g_frame_publish_fail_streak`: 240 кадров (~4 с) подряд без
  опубликованной камеры/позиции → `reset_world_caches()`, то есть любое
  «залипание» само чинится за несколько секунд;
* пустой результат `rebuild_marker_entities()` перепроверяется через 30 кадров
  (~0.5 с) вместо 180, чтобы метки возвращались сразу после догрузки мира.

### 3.6.7. Транспорт: мерцание бокса и «призрачные» копии игрока

Симптом: игрок садится в машину и едет — его бокс мерцает между тем местом, где
он сел, и текущим положением.

Причина двойная, поэтому и лечится с двух сторон:

1. **Позиция.** Все боксы строятся от `lastSavedPosition` (`PLAYER_POSITION`).
   У сидящего в транспорте игрока это поле перестаёт обновляться и держит точку
   посадки — движение идёт через транспорт (в дампе есть отдельный
   `MountTransformRecorder`). Теперь, если `vehicleID != 0`, позиция берётся из
   **отрисовываемого трансформа** игрока (`worldCameraRoot`, `PLAYER_TRANSFORM`
   @0x68) — он припарентен к сиденью и едет вместе с машиной. Иерархия
   раскручивается до корня (`read_transform_hierarchy_*`), то есть получается
   мировая позиция, а не локальная. От камеры до ног вычитается 1.60 м.
   Результат принимается, только если он похож на мировую координату.
2. **Дубликаты объектов.** В списке игроков нередко живут несколько
   `PlayerManager` на одного человека (объекты пулятся; это видно в старом
   `xvcen_esp_debug.log` — один и тот же `userID` на разных указателях).
   При посадке в транспорт старая копия остаётся стоять на месте посадки.
   Теперь объекты группируются по `userID` (@0x278) и рисуется только тот,
   который **шевелился позже всех** (`still_frames` — сколько кадров подряд
   позиция не менялась больше чем на 5 см). При равенстве выигрывает тот, кого
   рисовали в прошлом кадре (`g_player_track_pick`), иначе у припаркованной
   машины бокс прыгал бы между копиями. Копия локального игрока всегда
   проигрывает — так своя же «тень» не рисуется как чужой игрок.

`userID` перечитывается раз в ~2 с: объекты переиспользуются под других игроков.

### 3.6.5. Подбираемое с земли (`Oxide.ItemPickup`)

Всё, что валяется на земле, — `Oxide.ItemPickup : fNZ : Mirror.NetworkBehaviour`
(`dump.cs` строка ~202314). Читать инвентарь не нужно, класс сам несёт шортнейм
и количество:

| Константа | Смещение | Поле в dump.cs |
|---|---|---|
| `ITEMPICKUP_ITEM_OBJECT` | 0xA8 | `Oxide.Item <LAP>k__BackingField` |
| `ITEMPICKUP_SHORTNAME` | 0xD8 | `string item` |
| `ITEMPICKUP_AMOUNT` | 0xE0 | `int amount` |

Шортнейм переводится таблицей по подстроке (шортнеймы точечные:
`metal.fragments`, `low.grade.fuel`), специфичные записи стоят раньше общих:
`mushroom` → Грибы, `berry` → Ягоды, `cloth` → Ткань, `scrap` → Скрап,
`hq.metal` → Металл HQ, `metal.frag` → Фрагменты и ещё ~90 записей.

Если шортнейм в таблице не нашёлся, через **ту же таблицу** прогоняется
английское имя из игры (`Item → ItemData.m_Name`: «Blue Berry», «Metal
Fragments», «Stone Hatchet»), поэтому в таблице есть и написания с пробелами
(`metal frag`, `sheet metal`, `high quality`, `low grade`, `tech trash`).
Именно из-за этого раньше часть предметов (грибы, ягоды и т.п.) оставалась
по-английски: их шортнейм пишется иначе, чем ожидала таблица. Порядок записей
важен — оружие и инструменты стоят **до** ресурсов, иначе «Stone Hatchet» стал
бы «Камнем», а «pickaxe» — «Топором». Если не совпало и английское имя, оно и
показывается как есть, а если нет и его — сам шортнейм. При `amount > 1` к подписи
добавляется `x<кол-во>`, поэтому пилюли прореживаются **по позиции**, а не по
тексту (иначе две кучки ягод с разным счётом считались бы разными).

**Кусты и грядки** (куст ткани, грибные/ягодные кластеры) добываются ударами, а
значит приходят как `MineableObject`. После проверок руды у них дополнительно
просматривается лут (`gather_marker_from_loot`): `cloth`/`hemp` → Куст ткани,
`mushroom` → Грибы, `berry` → Ягоды, `pumpkin`/`corn`/`potato` → овощ. `wood`
не даёт ничего, поэтому деревья по-прежнему не рисуются.

Разбор имени — общая функция `for_each_name_token()`: имя режется по
разделителям, цифрам и camelCase-«горбам», сравнение идёт **по целым словам**
(поэтому `Crate` — не крыса, а `Ratchet` — не лут). Ею же пользуется
распознавание волков и крыс.

**Порядок распознавания животных:** сначала имя префаба, и только потом
`entityType`. У волка в префабе стоит чужой `entityType` (`Boar`), поэтому
раньше все волки подписывались «Кабан». Имя проверяется по целым словам, так
что руду или ящик оно зацепить не может.

### 3.6.4. Тиммейты и клан

`Oxide.PlayerManager` синхронизирует три строки (у всех трёх есть
`_Mirror_SyncVarHookDelegate_*`, значит они приходят на каждый клиент):

| Константа | Смещение | Поле |
|---|---|---|
| `PLAYER_TEAM_NAME` | 0x280 | `string teamName` |
| `PLAYER_CLAN_ID` | 0x290 | `string clanId` |
| `PLAYER_CLAN_TAG` | 0x298 | `string clanTag` |

Союзник = совпало непустое `teamName` **или** непустое `clanId` с локальным
игроком. Своя группа читается раз в кадр, чужие — вместе с ником/оружием
(раз в 30 кадров). `clanTag` показывается перед ником: `[ABC] Вася`.
`clanId` длиннее 31 символа, поэтому появился `read_managed_string_ex()` с
настраиваемым лимитом длины (обычный `read_managed_string()` — обёртка над ним).

При включённом тумблере «Тиммейты» союзники рисуются своим цветом **и
исключаются из аимбота**; выключишь тумблер — они снова обычные цели.
Трейсер до союзника всегда **зелёный** (`cfg::esp::ally_tracer_col`), независимо
от цвета обычных трейсеров — в этом весь смысл различения.

### 3.7. Aим / события игрока (ADS)
| Константа | Смещение |
|---|---|
| `EVENT_HANDLER_MANAGER_BACKREF` | 0xD0 |
| `EVENT_HANDLER_AIM_ACTIVITY` | 0x270 |
| `EVENT_HANDLER_LOOK_DIRECTION` | 0x140 |
| `SYNC_VALUE_OFFSET` | 0x20 |
| `ACTIVITY_ACTIVE_FLAG` | 0x10 |

`EVENT_HANDLER_*` и `ACTIVITY_ACTIVE_FLAG` лежат НЕ в `PlayerManager`, а в
классе по указателю `playerEventHandler` (0x78): он ротирует имя каждый билд
(`DqO`→`Gum`), и в его середину добавляют новые активности. Из-за вставленного
`KnockDoor` (0x188) `Aim` уехал 0x268→0x270, а по 0x268 встал `Jump`. В карте
(`tools/offsets/offsets_map.json`) эти записи идут через `via`, поэтому скрипт
пересчитывает их по имени поля, а не «на глаз» (см. журнал от 13 сентября 2026).

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

### 3.10. Автофарм: «крестик» (hit-streak marker)

Автофарм бьёт не по корпусу ресурса, а по светящемуся крестику: попадание в
него игра засчитывает в серию (`hitstreakIndex`) и даёт бонусный ресурс.
Крестик — отдельный объект, который игра создаёт сама, и его координаты лежат
в экстеншенах узла. Цепочка целиком (все адреса — из `libil2cpp.so` текущего
билда, VA−0x4000 = смещение в файле):

```
MineableObject.OnStartClient (0x648f4e8) -> ciQ (0x648c768) -> cik (0x648e6ec)
    cik: GetComponents<JE>(gameObject) -> QWD (0xE8)   [кеш готов при спавне]
MineableObject.SRl (0x6490918)  [локальный удар]       -> cik -> JE.gir(hitInfo)

руда   OreHitstreaks.gir (0x772d1ec):
         MoW == null -> giu (0x772a260, Collider.ClosestPoint по поверхности)
                     -> gil (Instantiate + SetParent + set_position) -> MoW
       OreHitstreaks.os (0x772c7f4)  [проверка попадания]:
         get_transform(MoW) -> get_position -> дистанция до точки удара
       OreHitstreaksMarker.Update (0x772f3bc): MTG += dt; MTG > 15 ->
         SetActive(false) -> giq (0x7729c90): Destroy(маркер), MoW = null

дерево TreeHitstreaks.gir (0x7733120):
         MTn == null -> raycast + giJ -> Instantiate (0x77335ec) -> MTn (0x50),
         точка попадания на коре -> MTQ (0x88), второй конец отрезка -> MTu (0xA4)
       TreeHitstreaks.giL (0x77313dc) и вся обвязка (rRS/DMU/Dne/DfW):
         дистанция от точки удара до ОТРЕЗКА MTQ..MTu, радиус 0.15 м (MTx=0.15,
         MTc=0.0225). Наведение ровно на MTQ даёт нулевую дистанцию до отрезка.
       HitMarkerItem.giI (0x7731728): mark.localPosition = точка + normal*0.25
         (декаль вынесен с коры, чтобы не z-файтил) — поэтому MTQ первична,
         а трансформ декаля (`HITMARK_MARK`) используется только как запасной.
```

| Константа | Смещение | Поле / откуда |
|---|---|---|
| `MINEABLE_EXTENSIONS` | 0xE8 | `MineableObject.QWD` — `JE[]`, кеш экстеншенов |
| `OREHS_STREAK_INDEX` | 0x20 | `OreHitstreaks.hitstreakIndex` (int) |
| `OREHS_MARKER_TEMPLATE` | 0x28 | спящий шаблон на пивоте узла (не цель!) |
| `OREHS_MARKER` | 0x30 | `OreHitstreaks.MoW` — живой клон-крестик |
| `OREHS_COLLIDER` / `OREHS_MINEABLE` | 0x38 / 0x40 | `Mom` / `MoK` |
| `OREMARK_RENDERER` / `OREMARK_OWNER` | 0x20 / 0x38 | `meshRenderer` / `MTD` (back-ref) |
| `OREMARK_SCALE` / `OREMARK_AGE` | 0x48 / 0x58 | `MTS` / `MTg` (секунды жизни, > 15 — гаснет) |
| `TREEHS_MOVING_METHOD` | 0x20 | enum `MovingMethod` (Static / AroundTree) |
| `TREEHS_MARKER_TEMPLATE` | 0x28 | `hitStreakMarkerOriginal` (`HitMarkerItem`) |
| `TREEHS_STREAK` | 0x48 | `MTz` (сбрасывается в `gie`) |
| `TREEHS_MARKER` | 0x50 | `MTn` — живой клон-крестик |
| `TREEHS_SPOT_A` / `TREEHS_SPOT_B` | 0x88 / 0xA4 | `MTQ` / `MTu` — Vector3, мировые |
| `HITMARK_LIFETIME` / `HITMARK_MARK` | 0x20 / 0x38 | `lifetime` / `mark` (Transform декаля) |

**Как найти заново после апдейта игры.** Имена классов обфусцированы, но
структура та же: ищем в `dump.cs` классы `MineableObjectExtension_*`
(`OreHitstreaks`, `TreeHitstreaks`, `OreHitstreaksMarker`, `HitMarkerItem`) —
они наследуют `JE` (интерфейс экстеншенов) и `MonoBehaviour` соответственно.
Порядок такой:

1. `MineableObject`: найти метод, который делает `GetComponents<JE>` и пишет
   результат в поле-массив (`cik`); поле = `MINEABLE_EXTENSIONS`.
2. В классе-наследнике `JE` для руды: метод с `Instantiate` + `set_position`
   (`gil`) пишет живой маркер — это `OREHS_MARKER`; тот же метод/обвязка пишет
   `hitstreakIndex` (`OREHS_STREAK_INDEX`).
3. В наследнике `JE` для деревьев: метод проверки попадания (`giL`) читает два
   `Vector3` и сравнивает дистанцию до отрезка с константой 0.15 — меньший
   offset = `TREEHS_SPOT_A`, больший = `TREEHS_SPOT_B`; рядом `ldr x?,[x?,#?]!`
   с null-проверкой — `TREEHS_MARKER`.
4. Мировые координаты читаются штатно: `managed_object_native()` ->
   `native_component_transform()` -> `marker_world_position()` (для дерева
   `TREEHS_SPOT_A` — уже готовый `Vector3`, трансформ не нужен).

Все значения продублированы в `tools/offsets/offsets_map.json`, поэтому
`python3 tools/offsets/update_offsets.py` после апдейта пересчитает их сам
(поля ищутся по имени, а обфусцированные — позиционным выравниванием типов).

**Сверка с билдом `62a8534` (сентябрь 2026): все 20 оффсетов крестика БЕЗ
ИЗМЕНЕНИЙ.** Раскладка `OreHitstreaks` / `OreHitstreaksMarker` /
`TreeHitstreaks` / `HitMarkerItem` / `MineableObject` совпала 1:1 по типам и
смещениям, имена самих классов (`MineableObjectExtension_*`) не изменились —
ротировали только обфусцированные имена полей и базовый интерфейс экстеншенов
(`JE` -> `dk`, на него код не смотрит):

| было | стало | константа |
|---|---|---|
| `QWD` | `LXZ` | `MINEABLE_EXTENSIONS` 0xE8 |
| `MoW` / `MoK` / `Mom` | `lzD` / `lzk` / `lzR` | `OREHS_MARKER` 0x30 / `OREHS_MINEABLE` 0x40 / `OREHS_COLLIDER` 0x38 |
| `MTD` / `MTS` / `MTg` | `lzF` / `lzA` / `lHG` | маркер руды 0x38 / 0x48 / 0x58 |
| `MTz` / `MTn` / `MTQ` / `MTu` | `lHX` / `lHe` / `lHC` / `lHl` | `TREEHS_STREAK` 0x48 / `TREEHS_MARKER` 0x50 / `SPOT_A` 0x88 / `SPOT_B` 0xA4 |

Семантика (а не только раскладка) перепроверена дизассемблером по новому
`libil2cpp.so`: `OreHitstreaks.Update` (0x786b260) по-прежнему перечитывает
0x30/0x40; у маркера `Update` (0x786eb40) таймер `0x58 += Time.deltaTime`
сравнивается с **15.0** и уходит в `Destroy`, обратная ссылка на экстеншен —
0x38; `0xE8` у `MineableObject` лениво заполняется `GetComponents<dk>`
(0x65677d0, запись через пред-индекс `str x0,[x22]`, где `x22 = this+0xE8`);
у дерева `0x50` читается 11 раз и зануляется дважды, `0x88`/`0xA4` пишутся
парой при пересчёте сегмента, `0x48` сбрасывается в ноль.

**Полная раскладка из `dump.cs` билда `62a8534` (строки 102853 и 103073).**
Важно для будущих апдейтов: часть имён полей НЕ обфусцирована, их можно искать
по имени, а не позиционным выравниванием типов.

`MineableObjectExtension_TreeHitstreaks : dk` — строка 103073:

| Смещение | Имя в дампе | Тип | Наша константа |
|---|---|---|---|
| 0x20 | `movingMethod` | `TreeHitstreaks.MovingMethod` (enum) | — |
| 0x28 | `hitStreakMarkerOriginal` | `HitMarkerItem` | спящий шаблон |
| 0x30 | `RandomMovingAmplitude` | `Vector2` | — |
| 0x38 | `RandomMovingMinDegrees` | `float` | — |
| 0x3C | `InEdit` | `bool` | — |
| 0x40 | `AxisPoints` | (тип обфусцирован) | — |
| 0x48 | `lHX` | `int` | `TREEHS_STREAK` |
| 0x50 | `lHe` | `HitMarkerItem` | `TREEHS_MARKER` |
| 0x58 | `lHZ` | `Vector3` | — |
| 0x64 | `lHO` | `Vector3` | — |
| 0x70 | `lHP` | `Vector3` | — |
| 0x7C | `lHS` | `Vector3` | — |
| **0x88** | **`lHC`** | **`Vector3`** | **`TREEHS_SPOT_A` (точка на коре)** |
| 0x94 | `lHW` | **`Quaternion`** (16 байт, 0x94..0xA3) | — |
| **0xA4** | **`lHl`** | **`Vector3`** | **`TREEHS_SPOT_B` (конец отрезка)** |
| 0xB0 | `lHi` | `LODGroup` | — |
| 0xB8 | `lHY` | (тип обфусцирован) | — |

Между `SPOT_A` (0x88) и `SPOT_B` (0xA4) лежит **кватернион** — то есть 0x88/0x94
читаются как поза (позиция + поворот). Это надо иметь в виду, если `SPOT_A`
когда-нибудь перестанет читаться: тип поля на 0x88 может оказаться не `Vector3`,
а частью позы, и тогда точку на коре придётся брать иначе.

`MineableObjectExtension_HitMarkerItem : MonoBehaviour` — строка 102853:

| Смещение | Имя в дампе | Тип | Наша константа |
|---|---|---|---|
| 0x20 | `lifetime` | `float` | `HITMARK_LIFETIME` |
| 0x28 | `mFilter` | `MeshFilter` | — |
| 0x30 | `renderer` | `Renderer` | — |
| **0x38** | **`mark`** | **`Transform`** | **`HITMARK_MARK` (декаль)** |
| 0x40 | `randomizeRotation` | `float` | — |
| 0x44 | `randomizeScale` | `float` | — |
| 0x48 | `audioSource` | `AudioSource` | — |
| 0x50 | `sound` | `Oxide.SoundPlayer` | — |
| 0x58 | `lzu` | `LODGroup` | — |
| 0x60 | `lzK` | `MaterialPropertyBlock` | — |
| 0xC8 | `lzr` | `float` (возраст) | `HITMARK_AGE` |

Поля `lifetime`/`mark`/`randomizeRotation` не обфусцированы, поэтому
`HITMARK_LIFETIME` и `HITMARK_MARK` после апдейта находятся поиском по имени.

Клиент/сервер: `PlayerInteraction.AvG` (0x657a360) — клиентский отправитель
`[Command]` c `MineableObjectHitInfoCompact`, `PlayerInteraction.ABi`
(0x656e120) — серверный обработчик. Крестик создаётся на КЛИЕНТЕ локальным
путём удара (`SRl`), поэтому читать его можно без всякой сетевой задержки.

### 3.11. Дальность удара ближним орудием (топор / кирка / пила)

**Как игра решает, попал ли удар** — `Oxide.FPMelee.ZkX` (RVA 0x6533a20 в билде
`62a8534`), дословно из дизасма:

```
handler = FPObject.PlayerEventHandler (0xC8)
data    = handler.RaycastData (0x160) -> value (+0x20)      // основной луч
if (!GKo.ZnJ(data))                                          // невалиден —
    data = handler.AimRaycast (0x168) -> value (+0x20)       // запасная сфера
if (!GKo.ZnJ(data)) -> On_Woosh()
if (data.RaycastHit.distance < m_MaxReach + hitRadius) -> On_Hit(data)
else -> On_Woosh()
```

`distance` — `UnityEngine.RaycastHit.get_distance()` (RVA 0xc87e3a4), то есть
**3D-метры от камеры/оси выстрела**, а не горизонтальное расстояние до узла.
Сравнение — `fadd s1, m_MaxReach, hitRadius; fcmp s0, s1; b.pl On_Woosh`.

| Константа | Смещение | Откуда / комментарий |
|---|---|---|
| `FPMELEE_MAX_REACH` | 0x128 | `FPMelee.m_MaxReach` — дальность удара орудия |
| `FPMELEE_HIT_RADIUS` | 0x12C | `FPMelee.hitRadius` — радиус сферы и добавка к дальности |
| `FPOBJECT_RAYCAST_MANAGER` | 0x90 | `FPObject.LUw` — компонент лучей (один на орудие) |
| `RAYCASTMAN_PLAYER` | 0x20 | back-ref на `PlayerManager` (проверка «свой») |
| `RAYCASTMAN_RAY_LENGTH` | 0x38 | `m_RayLength` — длина основного луча |
| `RAYCASTMAN_AIM_RAY_LENGTH` | 0x3C | `m_AimRayLength` — длина запасного `SphereCast` |
| `RAYCASTMAN_SPHERE_RADIUS` | 0x40 | радиус сферы (`LtS`) |
| `RAYCASTMAN_TOO_CLOSE` | 0x44 | порог «слишком близко» для `IsCloseToAnObject` |

**Самих чисел в дампе нет**: `m_MaxReach`, `hitRadius`, `m_RayLength`
сериализованы в префабе каждого инструмента. В конструкторах — заглушки:
`FPMelee..ctor` (0x6533b4c) пишет `m_MaxReach 0.5`, `hitRadius 0.1`,
`m_TimeBetweenAttacks 0.85`, `m_DamagePerHit 15`, `m_ImpactForce 15`;
`RaycastManager..ctor` (0x6553310) — `m_RayLength 1.5`, `m_AimRayLength 1.5`,
`m_TooCloseThreeshold 1.0`. Поэтому значение читается из живого орудия в руках:
`read_local_melee_reach()` в `game.cpp`, наружу — `FarmTarget.melee_reach`, а
панель цели показывает его («Дерево · 1.9 м · удар до 2.40»).

**Кто и что кастует** — `Oxide.RaycastManager.Update` (RVA 0x65559fc):

* основной `Physics.Raycast(ray, out hit, m_RayLength, m_LayerMask)` →
  активность `RaycastData` (0x160); луч строит `GuL.ZJP` (0x64e7924) из позы
  камеры через `GuL.ZJS` + `Camera.nearClipPlane`;
* запасной `Physics.SphereCast(ray, radius, out hit, m_AimRayLength,
  m_AimLayerMask, ...)` → `AimRaycast` (0x168), и кастуется **только если
  основной луч промахнулся** (`RaycastData.value == null`, проверка на
  0x6555ebc) и `m_AimRayLength > 0` и радиус > 0;
* при доставании орудия `FPMelee.On_Draw` (0x6533b14) кладёт свои `m_MaxReach`
  и `hitRadius` в `m_AimRayLength`/радиус сферы — `RaycastManager.ZIj`
  (0x6555868) это буквально `stp s0, s1, [x0, #0x3c]`.

Отсюда рабочая дальность удара: по основному лучу
`min(m_RayLength, m_MaxReach + hitRadius)`, а если он промахнулся — по сфере до
`m_MaxReach` вдоль луча плюс `hitRadius` бокового прощения (для сферы проверка
`distance < m_MaxReach + hitRadius` проходит всегда). Итоговый ориентир —
**`m_MaxReach + hitRadius` 3D-метров от глаза**; автофарм берёт его с запасом
0.90 (`kReachLiveTight`), гистерезис 0.98 (`kReachLiveHold`), а по корпусу без
крестика добавляет 1.20 м на радиус узла (`kReachBodyAllowance`). Если орудие не
опознано или значение не прочиталось, работают прежние эмпирические пороги
`kReach*` — метрика при этом горизонтальная (`aim_dist`), с живой — `aim_3d`.

**Классы ближнего орудия** (имена читаемые, между билдами не ротируют):
`Oxide.FPMelee` → `Oxide.FPTool` (`m_ToolPurposes` 0x160: CutWood=1,
BreakRocks=2, CutAnimals=4; `m_Efficiency` 0x164) → `Oxide.FPChainsaw`; рядом
`FPSpear`, `FPBuildingHammer`, `FPTorch`. Проверка имени класса обязательна: у
прочих `FPObject` на 0x128 свои поля — у `FPCrossbow`/`FPSnowball` там
`m_MaxDistance` (сотни метров), и без проверки бот решил бы, что арбалетом можно
рубить деревья с двухсот метров.

**Как найти заново после апдейта.** Якорь — читаемые имена `m_MaxReach` и
`hitRadius` в `Oxide_FPMelee_Fields` (`il2cpp_layout.py find m_MaxReach`), дальше
`Oxide_RaycastManager_Fields` (`m_RayLength`/`m_AimRayLength`). Все восемь
констант занесены в `offsets_map.json` с читаемыми именами полей, поэтому
`update_offsets.py` пересчитывает их сам; обфусцированы только `FPObject.LUw` и
`RaycastManager.LtS` — их ведёт позиционное выравнивание по типу.

### 3.12. Автофарм: что ещё отдаёт дамп (инструмент, живость крестика, луч, ритм)

Всё ниже сверено с дампом `62a8534` (`il2cpp.h` + дизасм `libil2cpp.so`) и
занесено в `offsets_map.json` (стало 175 записей).

**1. Чем узел вообще можно взять.** `MineableObject.m_RequiredToolPurpose`
(0x70) и `FPTool.m_ToolPurposes` (0x160) — один и тот же
`enum Oxide.FPTool.ToolPurpose`, `[Flags]`:

```
CutWood = 1, BreakRocks = 2, CutAnimals = 4     // dump.cs, TypeDefIndex 8943
```

Сравнение побитовое: `(purposes & required) != 0`. Иерархия
`FPMelee (поля до 0x158) -> FPTool (0x160 m_ToolPurposes, 0x164 m_Efficiency,
0x168 alwaysCrit, 0x169 useAmmo, 0x16C attacksPerAmmo) -> FPChainsaw`, поэтому
0x160 читаем **только** когда имя класса в руках `FPTool` или `FPChainsaw`: у
`FPSpear`/`FPBuildingHammer`/`FPTorch` (наследники `FPMelee`, не `FPTool`) на
этом месте свои поля. Значение дополнительно проверяется на «только известные
флаги», мусор считается отсутствием требования — иначе сбой чтения оставил бы
фарм без целей.

**2. Живость крестика.** «Поле маркера заполнено» ≠ «X ещё виден»: оба маркера
ведут собственный отсчёт и гаснут сами.

*Руда* — `OreHitstreaksMarker.Update` (RVA 0x786eb40), дословно:

```
ldr  s8, [x19, #0x58]     ; lHG — возраст
bl   Time.deltaTime
fadd s0, s8, s0
fmov s1, #15.0            ; срок жизни — литерал 15 секунд
fcmp s0, s1
str  s0, [x19, #0x58]     ; lHG += dt
b.le <жить>               ; <= 15 — крестик виден
  GetComponent -> gameObject.SetActive(false)
  ldr  x0, [x19, #0x38]   ; lzF — владелец OreHitstreaks
  b    0x7869a44          ; хвостовой вызов: владелец убирает маркер
```

*Дерево* — `HitMarkerItem.Update` (RVA 0x786485c):

```
ldr  s8, [x19, #0xc8]     ; lzr — возраст
bl   Time.deltaTime
fadd s0, s8, s0
ldr  s1, [x19, #0x20]     ; lifetime — ПОЛНЫЙ срок (из префаба, не 15 с)
fcmp s0, s1
str  s0, [x19, #0xc8]     ; lzr += dt
b.le <жить>
  ldr  x8, [x19, #0xc0]   ; lzT — владелец/пул
  ...  br x3              ; виртуальный вызов: маркер возвращается в пул
```

Объект из пула переиспользуется для другого дерева, поэтому возраст проверяется
до того, как поверить координатам (вторая линия защиты — `farm_spot_on_node()`,
которая не принимает точку далеко от своего узла).

| Константа | Смещение | Поле | Смысл |
|---|---|---|---|
| `OREMARK_AGE` | 0x58 | `lHG` (float) | секунды с появления X на руде |
| `OREMARK_LIFETIME` | — | литерал `15.0` в Update | срок жизни X на руде |
| `HITMARK_LIFETIME` | 0x20 | `lifetime` (float) | ПОЛНЫЙ срок жизни X на дереве |
| `HITMARK_AGE` | 0xC8 | `lzr` (float) | сколько уже прожито |
| `TREEHS_SPOT_A` | 0x88 | `lHC` (Vector3) | начало отрезка X на коре |
| `TREEHS_SPOT_B` | 0xA4 | `lHl` (Vector3) | конец отрезка X |

Расклад `OreHitstreaksMarker` целиком (для сверки после апдейта): 0x20
`meshRenderer`, 0x28 `ignoringLayerMask`, 0x30 `sizeByDistance`
(AnimationCurve), 0x38 `lzF` (владелец), 0x40 `lzN` (MaterialPropertyBlock),
0x48 `lzA` (плавное значение), 0x50 `lHh`, 0x58 `lHG` (возраст).

**3. Прицел в отрезок, а не в его начало.** Проверка попадания у дерева меряет
дистанцию до отрезка `MTQ..MTu` радиусом 0.15 м (§3.10), поэтому `farm_read_spot`
целится в ближайшую к глазу точку отрезка, если проекция ложится строго внутрь
(2%..98%) и она ближе конца A минимум на 15 см. Иначе остаётся A — так же, как
раньше.

**4. Перекрыт ли узел (луч игры вместо своего рейкаста).** Тот же источник, из
которого `FPMelee.ZkX` берёт `distance` (§3.11), даёт готовый ответ «что сейчас
видит прицел»:

```
handler = FPObject.PlayerEventHandler (0xC8, тип Gum)
data    = handler.RaycastData (0x160) -> GuI`1<GKo>.value (+0x20)
if (data == null) data = handler.AimRaycast (0x168) -> value (+0x20)
```

«Валидность» в `ZkX` — это просто `data != null`: геттер на 0x654fc58 целиком
состоит из `cmp x0, #0; cset w0, ne; ret`. Расклад `GKo` (класс результата
луча, имя ротирует — ищется по форме: `RaycastHit` на 0x48 и `GameObject` на
0x18):

| Смещение | Поле | Тип |
|---|---|---|
| 0x10 / 0x11 | `Lth` / `LtG` | bool |
| 0x18 | `Ltj` | `GameObject` — во что упёрся луч |
| 0x20 | `LtE` | `GKw` |
| 0x28 | `LtM` | `InteractionComponent` |
| 0x40 | `LtU` | `PlayerManager` |
| 0x48 | `Ltt` | `UnityEngine.RaycastHit` (0x2C байт) |
| 0x74 | `LtX` | `Vector3` |

`UnityEngine.RaycastHit`: `m_Point` 0x0, `m_Normal` 0xC, `m_FaceID` 0x18,
**`m_Distance` 0x1C**, `m_UV` 0x20, `m_Collider` 0x28.

Автофарм сравнивает `m_Distance` со своей `aim_3d`: луч упёрся более чем на
0.6 м раньше точки прицела — значит между игроком и крестиком камень/забор/склон
и удар уйдёт в перекрытие. Бот в этом случае не тапает, а уходит тем же
манёвром, что и при застревании (`kBlockedTime` 0.45 с дебаунса, дальше обход,
после `kEvadeMax` попыток узел в чёрный список).

**5. Ритм ударов — из орудия.** `FPMelee.m_TimeBetweenAttacks` (0x130) и
`pauseAfterAttack` (0x134); в конструкторе заглушки 0.85/0.15, настоящие
значения сериализованы в префабе. Контроллер тапает на 2% медленнее суммы:
ранний тап игра ставит в очередь, и он вылетает уже в уведённую камеру. Период
зажимается в 0.25..2.0 с, при нечитаемом значении остаются прежние 85/230 мс.

**6. Состояние узла.** `m_CurrentHealth` (0x78) и `m_MaxHealth` (0xC0) — самый
тонкий признак того, что удары доходят: `fractionRemaining` (0xD0) сдвигается на
проценты лишь спустя десятки попаданий, а здоровье падает уже от первого.
Watchdog автофарма теперь видит прогресс по трём независимым признакам
(здоровье, остаток, серия по крестику). `m_Experience` (0xD4) — только для
строки статуса. Рядом, но не читаем: `m_Resistance` (0x74).

**Как найти заново после апдейта.** Якоря читаемые: `m_RequiredToolPurpose`,
`m_ToolPurposes`, `m_TimeBetweenAttacks`, `pauseAfterAttack`, `m_CurrentHealth`,
`m_Experience`, `RaycastData`, `AimRaycast`, `m_Distance`
(`il2cpp_layout.py find <имя>`). Обфусцированы только `lzr`/`lHG` (возраст
крестиков) и поля `GKo` — их ведут позиционное выравнивание по типу и
дизасм `Update` обоих маркеров (`fmov s1, #15.0` у руды, `ldr s1, [x19, #0x20]`
у дерева).

---

## 4. Ключевые правила (не забудь после апдейта)

1. **Имя игрока (ник) читается ТОЛЬКО из `PlayerManager + 0x220`.**
   Это подтверждено в рантайме: строка по `0x220` == приватная строка никлейбла
   (`пахановский`, `#Фришка`, `dusterhuffer`, ...). Голосовые поля
   (`PLAYER_VOICE_STATE.NAME`, `voicePlayer.tag`) и `userID` дают только
   машинный код `932D3ABF57D64819` — **не используй их как имя**, максимум фолбэк.
2. `CAMERA_VIEW_MATRIX@0x70` / `PROJECTION@0xB0` — ленивые кеши; при внешнем
   чтении устаревают → ESP "плывёт". Для камеры перестраивай вью от живого
   `Transform@0x20`.
3. Пути к объектам старайся **валидировать back-ref** (равенство указателей
   обратно на игрока), чтобы не читать чужие/мёртвые объекты.
4. **Никогда не ищи структуру или поле по старому обфусцированному имени** —
   они перегенерируются каждым билдом. Сверяй позиционно (смещение + тип),
   а переименованный класс ищи по форме полей: `tools/offsets/il2cpp_layout.py`.
5. **`..._TYPEINFO_RVA` пересчитывай при каждом апдейте.** Если хоть один
   неверен, соответствующая ветка ридера тихо отключается: имя класса по
   кандидату не совпадёт, `g_*_class` останется 0 и ESP будет пустым.

---

## 4.1. Бета-версия игры (второй набор оффсетов)

У игры две версии — релиз и бета, — и раскладка структур у них разная. Бета не
пересчитывает релизный заголовок, а живёт рядом:

```bash
python3 tools/offsets/beta_offsets.py            # что отличается от релиза
python3 tools/offsets/beta_offsets.py --apply    # записать файл беты
python3 tools/offsets/beta_offsets.py --apply --so libil2cpp.so   # .so беты отдельно
sh tools/offsets/run.sh                          # стенд переключателя версий
```

Дамп беты часто приходит без `libil2cpp.so` (так было и со `dump_beta.7z` от
16.09.2026 — внутри только `il2cpp.h`, `dump.cs`, `script.json`). Без него не
пересчитать `*_TYPEINFO_RVA`, а с релизными RVA бета-клиент классов не находит,
поэтому такая бета в меню не предлагается (`kRvaFromDump = false`). Нужен
`libil2cpp.so` той же сборки: ключом `--so` или как `libil2cpp_beta.7z` в корне
(сжат так же, как релизный `libil2cpp.7z`).

В клиенте версия выбирается на стартовом экране и в «Опциях»; от неё зависят и
оффсеты, и пакет процесса, к которому клиент цепляется
(`kPackageRelease`/`kPackageBeta` в `main.cpp`). После смены версии клиент
переподключается сам (`esp_reset` + поток привязки).

Что скрипт посчитать НЕ может (и потому у беты взято как в релизе, об этом
написано в шапке её файла):

* раскладка IL2CPP/Unity (`IL2CPP_STRING_*`, `TRANSFORM_*`, `GAMEOBJECT_*`,
  `IL2CPP_LIST_*`, `DICT_*`) — из дампа игры не выводится;
* нативные смещения `UnityEngine.Camera` из `libunity.so` — если в бете другая
  сборка Unity, нужен её `libunity.so` и ручной перемер (§3.1);
* `*_TYPEINFO_RVA` — только если есть `libil2cpp.so` беты (в архиве, ключом
  `--so` или как `libil2cpp_beta.7z` в корне); иначе останутся релизные, и
  бета-клиент не найдёт классы — такая бета в меню не предлагается, а причина
  видна в карточке беты.

Имя структуры беты искать не нужно: обфускатор перекатывает его вместе с полями
(`GKo_Fields` → `RET_Fields`), поэтому пропавшую структуру скрипт подбирает по
форме — по смещениям и типам полей (перекатанные имена типов считаются одним
токеном). Если похожих несколько, он не угадывает, а просит посмотреть руками.

RVA классов берутся по отпечатку релиза — набору читаемых статик-полей;
счётчик обращений в него не входит (он меняется от любой правки кода, и из-за
него верный слот легко принять за чужой). Вместе с RVA скрипт сдвигает окно
скана `TOD_SCAN_RVA_BEGIN/END` (§3.10) на дельту RVA: окно зависит от места
таблицы metadata-usage, а не от раскладки структур, и без сдвига «Всегда день»
на бете молча не находит класс. Если дельты RVA разъехались (разброс больше
64 КБ), скрипт окно не трогает и пишет об этом предупреждением.

Порядок при обновлении: сначала релиз (`update_offsets.py --apply`), затем бета
(`beta_offsets.py --apply`) — бета считается от релизного дампа, и после
апдейта релиза её файл иначе описывает прошлый билд.

---

## 5. Процедура обновления после патча игры

Скрипты лежат в `tools/offsets/` (там же README с деталями). Вся процедура —
минут на десять.

```bash
tools/offsets/extract_dumps.sh /tmp/new              # свежие дампы из рабочего дерева
tools/offsets/extract_dumps.sh /tmp/old <старый-коммит>   # дампы до аплоада
python3 tools/offsets/il2cpp_layout.py --old /tmp/old/il2cpp.h --new /tmp/new/il2cpp.h diff
python3 tools/offsets/typeinfo_rva.py --so /tmp/new/libil2cpp.so --script /tmp/new/script.json \
        Oxide.PlayerManager Oxide.GameControllerBase Mirror.NetworkClient
```

1. **Раскладка структур.** `il2cpp_layout.py diff` проходит по всем структурам,
   на которых держится `game_offsets.h`, и печатает либо «раскладка не
   изменилась», либо конкретные разъехавшиеся смещения. По каждой изменившейся
   — `show <структура>` и ручное сопоставление полей.
2. **Сравнивай ПОЗИЦИОННО, а не по именам.** Имена классов и полей
   переобфусцируются каждый билд (`fvp`→`pmi`, `wK`→`ij`, `Mo`→`sR`), diff по
   именам врёт: он покажет «moved=0», молча пропустив переименованные поля.
   Переименованный класс ищется по форме: `il2cpp_layout.py find "WeaponBase_o*"
   "Oxide_WeaponPiece_o" "UnityEngine_Transform_o*"`.
3. **Смещения полей — только в `il2cpp.h`** (комментарии `/* 0xNN */`).
   В `dump.cs` этого дампера смещений нет, там полезны лишь значения enum'ов —
   их тоже надо сверять (`MineableEntityType`: значения могут не сдвинуться, но
   новые появляются).
4. **RVA `TypeInfo` уезжают ВСЕГДА** — без них ридер не стартует вообще
   (`resolve_runtime_player_list()` вернёт 0, ESP будет пустой). `ScriptMetadata`
   в `script.json` пустой, поэтому `typeinfo_rva.py` дизассемблирует методы
   самого класса: `adrp/ldr` → addend релокации `R_AARCH64_RELATIVE` → слот в
   `.data`, и оставляет только те слоты, которые потом разыменовываются как
   `ldr x8,[klass,#0xB8]` (`static_fields`) — ровно наш паттерн доступа.
   **Обязательно прогоняй тот же скрипт на старом дампе:** он должен
   воспроизвести значения, которые сейчас в git. Верхний кандидат обычно
   правильный, но у `GameControllerBase` его стабильно обгоняет чужой класс —
   верный слот тот, у которого в колонке статик-полей прочерк.
5. Нативные Unity-смещения не меняются между версиями Unity-рантайма — только
   если игра обновила сам Unity (`libunity.7z` в репозитории тот же блоб →
   раздел 3.1 не трогаем).
6. Пересобери: пушишь в рабочую ветку сессии → GitHub Actions соберёт
   `xvcen-sh-arm64-v8a` → забираешь артефакт из CI.
7. Проверка на устройстве: ESP показывает **реальные ники**, корректное оружие,
   боксы не сливаются. Если что-то не так — верни временный дамп полей из
   истории коммитов, таблица в разделе 6.

### 5.1. Что делает скрипт и что остаётся руками (с билда `62a8534`)

```bash
python3 tools/offsets/update_offsets.py            # сухой прогон: список изменений
python3 tools/offsets/update_offsets.py --apply    # записать в заголовок и карту
```

Автоматически: 111 полевых констант (по имени, а обфусцированные — позиционным
выравниванием типов) и 3 `*_TYPEINFO_RVA`. RVA теперь подбираются **по
отпечатку со старого дампа**, а не «верхним кандидатом»: скрипт ищет в старом
дампе тот кандидат, который равен текущему значению заголовка, запоминает его
отпечаток (число обращений + прочитанные статик-поля) и в новом берёт кандидат
с тем же отпечатком. Без этого `GameControllerBase` получает чужой слот
(0xD8E4CD8 вместо верного 0xD8D61E8) — ESP тихо умирает. Прогон с
`--methods 3000` обязателен: на 400 методах у `PlayerManager` в новом билде не
находится ни одного кандидата.

**Запустить `--apply` можно ровно один раз на дамп.** После него карта описывает
новый дамп, и повторный прогон сдвинет значения ещё раз (`PIECE`
0x100→0x110→0x120). Скрипт это ловит сам и останавливается («карта уже
описывает НОВЫЙ дамп»); если всё же испортил — `git checkout --
jni/src/game_offsets.h tools/offsets/offsets_map.json` и один прогон заново.

Откуда берётся каждый оффсет и чем он подтверждён — `tools/offsets/PROVENANCE.md`
(пересобирается `tools/offsets/make_provenance.py` из карты и заголовка).

Руками после скрипта (в карте помечены `kind: runtime`, их 40):

1. **Окно скана `TOD_SCAN_RVA_BEGIN/END`** (время суток). Это не поля, а
   диапазон `.data.rel.ro` с глобальными `Il2CppClass*`-слотами; уезжает каждый
   билд вместе с RVA. Пересчитать — см. комментарий в `game_offsets.h`
   (`typeinfo_rva.py ... UV`, оба кандидата обязаны попасть в окно).
   **Скрипт это не проверяет** — именно так в прошлом билде оно и протухло бы.
2. **Имена классов**, которые код сверяет в рантайме:
   `MineableObjectExtension_{OreHitstreaks,TreeHitstreaks}`, `Mineable*`,
   `LootObject`, `ItemPickup`, `LootDestroyable`, `PlayerManager`,
   `GameControllerBase`, `NetworkClient`, `NetworkIdentity`, `List\`1`.
   Быстрая сверка: `grep '^public class' new/dump.cs`.
3. **Нативные Unity/IL2CPP ABI** (`COMPONENT_GAMEOBJECT` 0x20,
   `TRANSFORM_CHILDREN_ARRAY` 0x48 / `CHILD_COUNT` 0x58, `ARRAY_LENGTH` 0x18 /
   `FIRST_ELEMENT` 0x20, `Il2CppClass.name` 0x10 / `namespace` 0x18 /
   `static_fields` 0xB8, раскладка `List`/`Dictionary`). Меняются только со
   сменой версии Unity. Дешёвая перепроверка по новому бинарю: в любом методе,
   читающем статики класса, должен быть `ldr x?,[x?,#0xb8]`, а в переборе
   интерфейсов — `ldrh w?,[x?,#0x12e]` + `ldr x?,[x?,#0xb0]` (в билде `62a8534`
   оба на месте, см. `OreHitstreaksMarker.Update` 0x786eb9c и `MineableObject`
   0x656781c).
4. **Таблица item-id → название оружия** (`weapon_label_for_item_id` в
   `game.cpp`). Она снята с устройства (`items.txt`, `/storage/emulated/0/
   benzhack`), а не из дампа, поэтому из дампов не проверяется. Если игра
   меняла базу предметов — снять заново; промах не фатален, unbekанный id
   уходит на фолбэк по имени префаба.
5. **`MineableEntityType`** (значения enum) — сверить в `dump.cs`: числа могут
   не сдвинуться, но новые члены появляются.

## 6. Как вернуть временную диагностику (если снова что-то сломалось)
В релизной сборке диагностики нет — она пишет на карту памяти, поэтому все
дампы удалены. При регрессе восстанавливай нужный из истории:

| Что дампилось | Функция | Файл на устройстве | Восстановить из |
|---|---|---|---|
| Поля игрока (ники, кости) | `dump_player_diagnostics()` + `diag_done` в `PlayerTextCache` | `xvcen_esp_debug.log` | `git show 08b5b6f` (удалена в `e1eb40c`) |
| Цепочка оружия удалённых игроков | `dump_weapon_probe()` + `weapon_probed` в `PlayerTextCache` | `xvcen_weapon_debug.log` | `git show eee29a6` |
| Обход реестра Mirror (руды/животные) | `dump_marker_probe()` + счётчики `probe_*` в `rebuild_marker_entities()` | `xvcen_marker_debug.log` | `git show 0aa7a3c` |
| Плашка `ESP attach=… pid=… boxes=…` слева сверху | `DrawAttachStatus()` + вызов перед `DrawEspOverlay()` в главном цикле | на экране | `git show 0f1f869` |
| Работа аимбота покадрово (чувствительность, ответ камеры на палец, скорость цели, остаток ошибки) | `AimDebugLog()` + `AimDebug`/`g_aimDbg` в `main.cpp`, выключатель «Отладка аима» | `xvcen_aim_debug.log` | `git show ac1270f` |
| **Автофарм покадрово (не удалён — живёт в сборке)** | `namespace farmlog` в `main.cpp`, тумблер «Отладка → Лог фарма» | `Загрузки/farm_debug.log` | — (см. §6.1) |

Все остальные — одноразовые (пишут при первом скане/первых 8 игроках), вызов
ставится рядом с местом, где значение уже посчитано. Лог автофарма — постоянный,
про него ниже.

### 6.1. Лог автофарма (`farm_debug.log`)

Автофарм — единственная часть, где «на глаз» не разобраться: бот водит тремя
пальцами сразу, а причина дёрганья может быть в подборе цели, в доводке камеры,
в зоне джойстика или в ритме тапов. Поэтому каждый его кадр пишется в лог.

**Куда.** `/storage/emulated/0/Download/farm_debug.log` (Загрузки). Если доступ
к общему хранилищу закрыт, пробуются `/sdcard/Download/` и каталог конфигов —
настоящий путь написан в шапке файла и в подсказке окна автофарма.

**Сколько.** Строка ~245 байт, на 60 к/с это ~15 КБ/с. Набрав 8 МБ файл
закрывается и уезжает в `farm_debug.1.log`, так что под рукой всегда последние
~9 минут. Пишется только пока автофарм включён; кадры без цели — 4 строки в
секунду. Буфер 16 КБ, сброс раз в 0.15 с; строки `EV` сбрасываются сразу.

**Два рода строк.**
* Числовая строка — состояние кадра: 44 ровных столбца, шапка с описанием
  каждого печатается в самом файле (формат тот же, что был у
  `xvcen_aim_debug.log`, чтобы читать глазами и резать `awk`).
* `EV <t_s> текст` — решение контроллера: смена узла, появление/пропажа
  крестика, застревание и обход, перекрытый узел, отказ от узла без прогресса,
  сброс коэффициента камеры, палец камеры на краю экрана, пауза (меню/аимбот),
  причина простоя, остановка фарма. Плюс два диагностических события (добавлены
  14.09.2026 после разбора «бьёт в воздух»):
  * `замах: ...` — по одному на каждый тап удара. Несёт геометрию: `dist`/
    `aim3d`, остаток ошибки камеры, `at`/`spot`/`life`/`strk`/`hp`/`blk`,
    состояние луча игры (`луч <м> (<±м> к прицелу) точка x,y,z (<м> от прицела)
    n x,y,z` либо `ЛУЧА НЕТ - удар не засчитается (On_Woosh)`), мировые
    координаты точки прицела и пивота узла, и сколько секунд подряд нет луча.
  * `крестик появился: ...` — теперь с сырыми полями сегмента дерева:
    `SPOT_A x,y,z SPOT_B x,y,z len <м> why <0..3>` (0 принят, 1 не конечен,
    2 вне узла, 3 не читали) и координаты прицела/пивота. По `why` видно, пусты
    ли поля в памяти или значения есть, но их отвергла проверка «точка на узле».

**Порядок чтения.**
1. `ex` — отработал ли кадр вообще (0 да; 1 выключен, 2 нет экрана, 3 нет цели,
   4 цель мигнула, 5 пауза, 6 пауза смены узла, 7 узел добыт).
2. Движение: `ww`/`slen`/`sdir` (команда стику) против `mv_dps`/`mv_dir`
   (куда реально пошёл персонаж — производная позиции глаза, `mv_dir`
   относительно камеры). Расходятся — зона джойстика мимо или раскладка не та;
   `mv_dps ~ 0` при `slen > 0` — упёрся.
3. Камера: `yaw`/`pitch` (остаток ошибки) против `ldx`/`ldy` (сколько послали)
   и `gain` (град/px). Ошибка не убывает — коэффициент выучен неверно.
4. Удары: `tp`/`tapms` (ритм) против `hp`/`frac`/`strk` (засчитывает ли игра)
   и `blk`/`ray` против `aim3d` (перекрыт ли узел). **`ray` = -99 при живом
   крестике — удар не засчитается вовсе**: `FPMelee.ZkX` берёт `distance` из
   обёртки `GKo` (`RaycastData`/`AimRaycast`), а с пустой обёрткой играет один
   `On_Woosh`. Контроллер такие тапы держит (до 2.5 с) и поджимает бота ближе к
   стволу — на 0.5 м луч живёт в 97% кадров, на 0.75 м в 19%.

**Где в коде.** `namespace farmlog` в `main.cpp` (строка кадра, события, файл и
ротация); `UpdateFarm` — обёртка, которая начинает кадр чистым и пишет его на
выходе, каким бы путём `UpdateFarmInner` ни вернулась (early return'ов там
с десяток, и обычно именно они и есть ответ на «бот стоял»). Значения в строку
пишет сам контроллер по ходу дела (`fl.` — ссылка на `farmlog::g_row`).
Позицию глаза для `mv_dps`/`mv_dir` отдаёт `esp_local_eye_position()`
(`game.cpp`: точка выстрела KCC → поза камеры → базис из матрицы вида; каждый
источник ещё и проверяется `farm_cam_source_ok()` — позиция обязана быть рядом
с корнем игрока, иначе нулевой вектор уводил `aim3d` на километр). Углы камеры
для обучения коэффициента — `esp_camera_angles()` с той же цепочкой. Сырые поля
крестика для строки `крестик появился` отдаёт `esp_farm_spot_raw()`.

## 7. Журнал апдейтов игры

### Проверка всей таблицы скриптом (сентябрь 2026)

`tools/offsets/update_offsets.py` сверяет каждую константу заголовка с полем,
за которое она отвечает, в обоих дампах сразу. Первый же прогон нашёл ошибку,
которая жила в таблице давно и не была связана с апдейтом:

| Константа | Было | Стало | Причина |
|---|---|---|---|
| `PLAYER_POSITION` | 0x1D0 | **0x1D4** | `lastSavedPosition` лежит по 0x1D4 и в старом дампе, и в новом; 0x1D0 — последние 4 байта соседнего `lastTickPosition`, то есть читалась смесь `tick.z, saved.x, saved.y` |

Промах не бросался в глаза, потому что позиция игрока берётся прямым путём
(`g_use_direct_player_position`), а это поле — запасной, и его проверка
(`position_looks_like_world_space`) такую смесь отбраковывала.

### Апдейт от сентября 2026 (дампы `dump.7z` / `libil2cpp.7z` в коммите `c0f5c80`)

Типов стало 31035 (было 30872), методов 274178 (было 269315). Из 28 структур,
на которых держится ридер, раскладку поменяли **четыре**, из них значимы две.

| Константа | Было | Стало | Причина |
|---|---|---|---|
| `PLAYER_MANAGER_TYPEINFO_RVA` | 0xD48CFB0 | **0xD7E4310** | новый билд libil2cpp.so |
| `GAME_CONTROLLER_TYPEINFO_RVA` | 0xD4884E8 | **0xD7DF6C8** | то же |
| `NETWORK_CLIENT_TYPEINFO_RVA` | 0xD48C270 | **0xD7E35B8** | то же |
| `LOOTOBJECT_BUILDING_PIECE` | 0xF0 | **0xF8** | в `LootObject` вставлено `System.String m_ContainerSoundKey` @0xE8 |
| `PLAYERWEAPON_VIEW` | 0x90 | **0xD0** | в `PlayerWeapon` вставлено 0x40 байт новых полей перед `animator` |
| `PLAYERWEAPON_PIECE` | 0xD8 | **0x100** | тот же сдвиг (SyncVar `weaponPiece`) |
| `PLAYERWEAPON_STATE` | 0xE8 | **0x110** | тот же сдвиг |
| `PLAYERWEAPON_PLAYER_BACKREF` | 0x100 | **0x128** | тот же сдвиг |

Что важно помнить по этому апдейту:

* `LOOTOBJECT_BUILDING_PIECE` был **живым багом в отгруженной сборке**: по
  старому 0xF0 теперь лежит `bool onlyGive`, то есть признак «деплой vs мировой
  контейнер» читался мусором. `panelName` (0xE0) стоит до вставки и не поехал.
* В новой раскладке `PlayerWeapon` есть **второй** `WeaponPiece` по 0xA8 — это
  не SyncVar, брать нельзя. Троица SyncVar'ов опознана позиционно по хвосту
  `_Mirror_SyncVarHookDelegate__loaded/_weaponPiece/_weaponState` (0x140/0x148/0x150).
* Класс view оружия переименован `Mo` → `sR` (найден по форме полей), но
  смещения внутри те же: 0x48 / 0x50 / 0x60.
* Обфусцированные классы вокруг игрока тоже переименованы —
  `fvp`→`pmi` (event handler), `wK`→`ij` (nicklabel), `fuI`→`pJk` (voice),
  — но раскладка у всех трёх идентична, константы не тронуты.
* Без изменений подтверждены: `PLAYER_POSITION 0x1D0` (в дампе по-прежнему
  `lastSavedPosition 0x1D4`, у боевой сборки блок сдвинут на −4 — **не менять**),
  весь блок `PLAYER_*` (userID 0x278, vehicleID 0x288, seatID 0x28C, клан
  0x290/0x298), `MINEABLE_*`, `ITEMDATA_NAME/SHORTNAME`, `ItemPickup`, `KCC`,
  `HitBox`, `Ragdoll`, `FP*`, `NetworkIdentity`, статик-поля (0x10 / 0x10 / 0x28)
  и цепочка камеры (`GameControllerBase.<ukA>` 0x38 → `CameraManager.m_Camera` 0x20).
* Мелочи без последствий: в `ItemData` добавлено `backpackConfig` @0xC0 (наши
  0x18/0x20 до вставки), у `MineableObject` поле 0xE0 сменило тип на
  `MineableRewardCalculator` — смещения те же.
* `MineableEntityType`: значения 0..22 не сдвинулись, добавились
  **`LootboxBaloon = 23` и `LootboxBaloonBig = 24`** (воздушные ящики) — оба
  заведены в enum и рисуются маркером ящика.
* `libunity.7z` и `moggerware.7z` — те же блобы, нативные Unity-смещения (§3.1,
  §3.9) не пересматривались.

### Апдейт от 7 сентября 2026 (новые `dump.7z` / `libil2cpp.7z`, коммиты `2d1e09c`/`00279ed`)

Прогон `update_offsets.py` по свежим дампам:

* **Все 87 проверяемых полей структур — без изменений** (раскладка классов не
  поехала, обфусцированные имена не пересматривались).
* Переехали только три TypeInfo-RVA (новый билд `libil2cpp.so`), пересчитаны
  дизасм-скриптом `typeinfo_rva.py` и записаны `--apply`:
  * `PLAYER_MANAGER_TYPEINFO_RVA` 0xD7E4310 → **0xD7AAAF8**
  * `GAME_CONTROLLER_TYPEINFO_RVA` 0xD7DF6C8 → **0xD7B4390**
  * `NETWORK_CLIENT_TYPEINFO_RVA` 0xD7E35B8 → **0xD7A9DC8**
* Рантайм-константы (`kind: runtime`, Unity/IL2CPP и камера из libunity) по
  обыкновению не проверялись; `libunity.7z`/`moggerware.7z` без изменений.

### Ручная сверка того же апдейта (без скрипта)

По требованию — полная ручная проверка по свежераспакованным `dump.cs`/`il2cpp.h`/`script.json`:

* **Поля, сверенные вручную по dump.cs — все на своих местах:**
  `PlayerManager` (worldCameraRoot 0x68, inventory 0x98, vitals 0xC8, weapons
  0x198, QHo 0x220, voice 0x2E8, SyncVar-блок 0x278–0x298, статики 0x8/0x10),
  статики `GameControllerBase` (0x8/0x10/0x38), `CameraManager.m_Camera` 0x20,
  `NetworkClient.spawned` 0x28, `NetworkIdentity` (netId 0x58, behaviours 0x80),
  `MineableObject` (0x78/0xA0/0xA8/0xC0/0xD0/0xD8), enum EntityType 0..24,
  `LootObject` (0xA0/0xA8/0xE0/0xF8), `ItemPickup` (0xA8/0xD8/0xE0),
  `ItemData` (0x18/0x20), `KCC` (0x70/0x78/0x88/0xA0/0xA4),
  `HitBoxRecorderRoot.hitBoxes` 0x68, `PlayerWeapon` (0xD0/0x100/0x110/0x128,
  хвост SyncVar-хуков 0x140/0x148/0x150), `FPManager` (0x50/0x58/0xA8),
  `PlayerModelInfo` (0x20/0x28/0x30/0x38).
* **Три TypeInfo-RVA подтверждены независимым дизасмом**: собственные методы
  каждого класса дизассемблированы capstone'ом, собраны GOT-слоты по паттерну
  `adrp+ldr → ldr [klass,#0xB8]` (доступ к static_fields), затем эти слоты
  разnamed через `R_AARCH64_RELATIVE` (addend = слот класса):
  * PlayerManager: GOT 0xd48ed30 → **0xD7AAAF8** (19 голосов, единственный кандидат)
  * GameControllerBase: GOT 0xd493b68 → **0xD7B4390** (42 голоса, топ)
  * NetworkClient: GOT 0xd4772b0 → **0xD7A9DC8** (48 голосов, топ)
* Переобфускация имён классов без смены раскладки: `pmi`→`DqO` (event handler,
  manager 0xD0 / Aim 0x268), `ij`→`OS` (nicklabel, 0x20/0x38), `pJk`→`DEY`
  (voice, 0x78), `pFG`→`DoI` (equipment), view оружия `sR`→`ey` (0x48/0x50/0x60
  на месте, проверено `il2cpp_layout.py find`), `PlayerWeapon` переехал в
  namespace `HyperHug...Features.Weapons` — раскладка не тронута.
* `PLAYER_POSITION 0x1D0` не менять: в dump.cs по-прежнему `lastSavedPosition
  0x1D4`, у боевой сборки блок сдвинут на −4 (см. правило в §4).

### Исправление после жалобы «ESP не работает»: GAME_CONTROLLER_TYPEINFO_RVA

Скрипт выбрал НЕ ТОТ кандидат для GameControllerBase (0xD7B4390 — чужой класс
с похожим профилем доступов). Ручной дизасм статических геттеров дал точный
ответ; сигнатура сверена со старым дампом:

* старый `get_localPlayer` (ygL @0x6575be0): слот 0xd4af648 → реллок
  **0xD7DF6C8** — совпадает со старым значением заголовка, метод верен;
* новый `get_localPlayer`/`get_cameraManager`/`get_netIdentity`
  (cKs/cKq/cKc): все трое читают слот 0xd477308 → реллок **0xD7A5E10** и
  затем `[static_fields+0x10/0x38/0x8]` — ровно наши поля.

`GAME_CONTROLLER_TYPEINFO_RVA`: 0xD7B4390 → **0xD7A5E10** (записано).

Два других RVA подтверждены адресно (доступ к нужному статик-полю):
* PlayerManager: слот 0xd48ed30 → 0xD7AAAF8, 10 доступов к `[sf+0x10]`
  (clientPlayerList) — верно;
* NetworkClient: слот 0xd4772b0 → 0xD7A9DC8, 3 доступа к `[sf+0x28]`
  (spawned) — верно.

Дифф раскладок старый↔новый (`il2cpp_layout.py diff`): 27 из 28 структур без
изменений; единственный «дифф» в ItemData — переименование типа фраз
(hQ_Phrase→Iv_Phrase), смещения те же. Наши константы полей корректны.

Урок в копилку §4: у GameControllerBase верный слот — тот, из которого читают
геттеры cK*/yg* (три подряд, 0x8/0x10/0x38); голосование по количеству
обращений выбирает чужой класс.

### Проверка нового libunity.so (тот же апдейт)

Пользователь залил новый `libunity.7z` (7366251 → 7404768 байт; предыдущий
коммит с оффсетами случайно откатил блоб — возвращён отдельным коммитом).
Ручная сверка старого и нового `libunity.so` (оба Unity **6000.3.18f1**):

* Бинарь пересобран — функции переехали (эталонная rebuild-матрицы
  `0xe2b90c` → `0xe2b3cc`, найдена по байтовой сигнатуре пролога). Мы
  адреса функций libunity не используем — только раскладку объекта Camera,
  поэтому сам по себе переезд ничего не ломает.
* Дизасм новой rebuild-функции подтверждает **все** наши смещения Camera:
  view-кэш `+0x70`, worldToClip `+0xF0` (те же `add x1,x19,#0x70` /
  `add x2,x19,#0xf0`), dirty-байты `+0x502`/`+0x500`, `nearClip 0x454`,
  `farClip 0x458`, `aspect 0x4E0`, transform `+0x20`.
* Счётчики кодированных обращений по всему бинарю (ldrb/ldr s/ldr q с нашими
  imm12) старый≈новый: 0x454=21/21, 0x458=25/25, 0x4E0=19/19, 0x170=52/52,
  0x502=17/17 (ldrb) и 13/13 (strb). `projection 0xB0` и `prevVP 0x5C8`
  сверены через ADD-immediate (2056/2042 и 77/78 — в пределах шума).
* Итог: **раскладка Camera в libunity не менялась, все нативные константы
  (§3.1/§3.9) действительны, править нечего.**

### Проверка libunity.so беты (16 сентября 2026, коммит `31b6840`)

Пользователь залил `libunity_beta.7z` (7 405 397 Б, блоб `18a2fd60`). Бета — это
отдельный бинарь (24 921 920 Б против 24 919 632 Б, `.text` 18 561 328 против
18 559 408), поэтому «скопировать смещения» нельзя: сверил обе сборки скриптом
`tools/offsets/unity_layout.py`.

* Версия Unity в обеих сборках одна и та же — **6000.3.18f1**, а раскладку
  Camera задаёт именно она.
* Функция пересборки матриц камеры (релиз `0xe2b3cc`, бета `0xe2b5dc`) найдена по
  отпечатку в коде: читается dirty-байт view-кэша `[this+0x502]`, затем пишутся
  view-кэш `[this+0x70]` и worldToClip `[this+0xf0]` от того же this.
* Кластер функций камеры (704 слова вокруг неё): **677 слов совпали байт-в-байт**,
  27 — переезд адресов (вызовы и пары adrp+add), содержательных отличий **0**.
* «Редкие» счётчики по всему `.text` (только обращения с базой-регистром, то есть
  к полям объекта): `ldr s` 0x454 = 20/20, 0x458 = 25/25, 0x4E0 = 19/19,
  0x170 = 32/32, `ldrb` 0x502 = 17/17, `strb` 0x502 = 12/12. В сверке 12 сентября
  для 0x454 и 0x170 числа были 21 и 52 — там в счёт попадали `[sp, #0x454]` и
  `[sp, #0x170]`, то есть кадры стека чужих функций; здесь правило строже.
* Итог: **раскладка Camera в бете не менялась — нативные константы §3.1 (и §3.9)
  действительны и для беты**, править их не нужно.

### Апдейт от 13 сентября 2026 (новые `dump.7z` / `libil2cpp.7z`, коммиты `d22586f`/`d3bb574`/`62a8534`)

Дампы: `dump.7z` sha256 `2dc425af…4fa1` (24 889 304 Б), `libil2cpp.7z` sha256
`57f9f051…06a1` (23 546 006 Б). Прошлые дампы — `89e0b63` (`dump.7z`
24 731 290 Б, `libil2cpp.7z` 23 378 070 Б). Идентичность поколения backing-полей
в новом билде — `L*` (в `89e0b63` было `Q*`, до того `_uk*_`), смещения те же.

**Что изменилось (7 констант):**

| Константа | было | стало | как найдено |
|---|---|---|---|
| `PLAYER_MANAGER_TYPEINFO_RVA` | 0xD7AAAF8 | **0xD8DB8B8** | отпечаток: 5 обращений, статик-поля `[0x1A0x1]` |
| `GAME_CONTROLLER_TYPEINFO_RVA` | 0xD7A5E10 | **0xD8D61E8** | отпечаток: 1 обращение, статик-полей нет |
| `NETWORK_CLIENT_TYPEINFO_RVA` | 0xD7A9DC8 | **0xD8DAB08** | отпечаток: 6 обращений, статик-полей нет |
| `PLAYERWEAPON_VIEW` | 0xD0 | 0xE0 | по имени `playerWeaponViewReference` |
| `PLAYERWEAPON_PIECE` | 0x100 | 0x110 | позиционно (`Oxide_WeaponPiece_o`) |
| `PLAYERWEAPON_STATE` | 0x110 | 0x120 | позиционно (`int32_t` за piece) |
| `PLAYERWEAPON_PLAYER_BACKREF` | 0x128 | 0x138 | по имени `_player_k__BackingField` |

Плюс вручную: `TOD_SCAN_RVA_BEGIN/END` 0xD7A0000/0xD840000 → **0xD8D0000/0xD970000**.

**Что НЕ изменилось:** все 20 оффсетов автофарма-крестика (§3.10), `PlayerManager`
(129 полей), `KCC`, `CharacterAnimation`, `Ragdoll`, `HitBox`, `Item`, `LootObject`,
`ItemPickup`, `FPObject`, `FPWeaponBase`, `FPManager`, `WeaponPiece`,
`NetworkIdentity`, статики `NetworkClient`/`GameControllerBase`, `TimeOfDay`,
`TOD_CycleParameters`, весь блок нативных Unity/IL2CPP ABI.

**Ловушки этого апдейта (все три стоили бы молча сломанного чита):**

1. **`GAME_CONTROLLER_TYPEINFO_RVA` — верхний кандидат ВРЕТ.** `typeinfo_rva.py`
   на новом дампе выдаёт 0xD8E4CD8 (16 обращений, статик-поля `[0x8x8, 0x0x8]`)
   и 0xD8D61E8 (1 обращение, прочерк). Верный — второй; прогон на старом дампе
   это подтверждает (там та же пара: 0xD7B4390 чужой против 0xD7A5E10 верного,
   и 0xD7A5E10 — значение из git). `update_offsets.py` раньше брал `--top 1`,
   то есть записал бы чужой слот → `resolve_local_player()` вернул бы 0 →
   пустой ESP и мёртвый фарм. Теперь скрипт подбирает по отпечатку со старого
   дампа (см. §5.1).
2. **`PLAYER_MANAGER_TYPEINFO_RVA` не ищется на 400 методах.** В новом билде
   первые 400 методов `Oxide.PlayerManager` в порядке `script.json` не содержат
   чтения статик-полей — «кандидатов нет». С `--methods 3000` находится
   0xD8DB8B8, и ровно он же воспроизводится на старом дампе (0xD7AAAF8 из git).
3. **Окно скана TOD_Sky захардкожено в `game.cpp`, а не в заголовке.**
   `s_scan_rva = 0xD7A0000` / `kScanEnd = 0xD840000` — RVA прошлого билда;
   в новом в этом окне осталось 35 слотов вместо 82 154, то есть «всегда день»
   не нашёл бы инстанс никогда. Окно уехало ровно на +0x130000 (в новом
   0xD8D0000..0xD970000 — 82 167 слотов, оба кандидата TOD_Sky 0xD8DF4C8 и
   0xD8DFC98 внутри). Константы перенесены в `game_offsets.h`
   (`TOD_SCAN_RVA_*`) и занесены в карту, чтобы следующий апдейт их увидел.
   Заодно удалены мёртвые `TOD_TYPEINFO_RVA_CANDIDATES` / `TOD_STOP_TIME` /
   `TOD_CURRENT_HOUR` / `TOD_DAY_DURATION` / `TOD_NORM_TIME`: их никто не
   читал с тех пор, как day/night переведён на TOD_Sky, а устаревали они молча.

**Отдельно проверено руками (не скриптом):**

* `PlayerWeapon` разъехался на 41 смещение (вставлены поля около 0x68), поэтому
  позиционное выравнивание перепроверено глазами: old `nwY`(0x100,
  `Oxide_WeaponPiece_o`)→new `CxH`(0x110), old `nwt`(0x110,int32)→new
  `Cxn`(0x120), old `_player_k__BackingField`(0x128)→new(0x138); Mirror-делегаты
  `__weaponPiece`/`__weaponState` уехали на те же +0x10 (0x148→0x158,
  0x150→0x160), порядок piece→state сохранён.
* Читаемые имена полей у `PlayerWeapon` в этом билде ВПЕРВЫЕ обфусцированы
  (`playerWeaponViewReference`→`Cxm` на 0xD0 в старом дампе — это другая
  проверка; в новом `playerWeaponViewReference` снова читаемое на 0xE0).
  Страховка скрипта «читаемые имена не ротируют» на этом билде дала сбой на
  8 повёрнутых `_k__BackingField` — `_XXXX_k__BackingField` теперь считается
  обфусцированным именем (`BACKING_RE` в `update_offsets.py`).
* Имена классов крестика в новом `dump.cs` на месте
  (`MineableObjectExtension_{OreHitstreaks,TreeHitstreaks,OreHitstreaksMarker,HitMarkerItem}`),
  базовый интерфейс ротировал `JE`→`dk` (код его не использует).
* Единственное имя класса, исчезнувшее между билдами, — `DVL`; в коде это
  только строка-лейбл в таблице-канонизаторе `kWeaponNames`, совпадением имени
  класса оно не было, так что ничего не сломалось.

### Хотфикс 13 сентября 2026: не работал тумблер «Только в прицеле»

Жалоба: после апдейта оффсетов (`49e6dc9`) переключатель «Только в прицеле»
(`aim_scope_only`) перестал срабатывать — аимбот с включённым тумблером не
включался вовсе.

**Причина — одна константа.** Класс обработчика событий игрока ротирует имя
каждый билд (`DqO`→`Gum`), и в новый билд ему в середину добавили поле
`KnockDoor` (0x188). Всё, что идёт после, съехало на +8:

| поле | старый билд | новый билд |
|---|---|---|
| `manager` | 0xD0 | 0xD0 |
| `LookDirection` | 0x140 | 0x140 |
| `KnockDoor` | — | **0x188** (новое поле) |
| `InteractContinuously` | 0x188 | 0x190 |
| `Jump` | 0x260 | 0x268 |
| `Aim` | **0x268** | **0x270** |
| `Voice` | 0x270 | 0x278 |

`EVENT_HANDLER_AIM_ACTIVITY` остался 0x268, поэтому `read_local_aim_state()`
читал активность **прыжка** вместо прицеливания: флаг почти всегда ноль →
аимбот в прицеле не активировался никогда. Значение исправлено на 0x270.

**Почему скрипт это пропустил.** Две независимые дыры, обе закрыты:

1. **Карта описывала не тот класс.** Четыре записи
   (`EVENT_HANDLER_AIM_ACTIVITY`, `EVENT_HANDLER_MANAGER_BACKREF`,
   `EVENT_HANDLER_LOOK_DIRECTION`, `VOICE_PLAYER_TAG`) были привязаны к
   `Oxide_PlayerManager_Fields` — структуре, где лежит УКАЗАТЕЛЬ, а не к классу,
   в котором живут сами поля. Проверка «у `PlayerManager` есть поле по 0x268»
   истинна всегда, так что сдвиг внутри хендлера был невидим. Вдобавок `--apply`
   при «освежении имён» подставил в provenance то, что реально лежит по этим
   смещениям в `PlayerManager` (`stats`, `foreignCupboardZoneIntrusion`,
   `voicePlayer`, `playerEventHandler`) — карта стала выглядеть правдоподобно и
   врать одновременно.
   **Решение:** в карту добавлен `via` — путь от стабильной структуры по
   читаемым именам полей (`playerEventHandler` → `Aim`). Скрипт разрешает его в
   имена структур обоих дампов и проверяет константу как обычное поле; при
   `--apply` имя структуры перезаписывается текущим. Переведены все 7 записей,
   живущих за указателем: три `EVENT_HANDLER_*`, `ACTIVITY_ACTIVE_FLAG` (два
   шага: `playerEventHandler` → `Aim` → `_LEb_k__BackingField`),
   `VOICE_PLAYER_TAG`, `NICKLABEL_PLAYER_BACKREF`, `NICKLABEL_NICKNAME_TEXT`.
2. **Эвристика «короткое имя = обфусцированное».** `Aim`, `Jump`, `Voice`
   попадали под `^[A-Za-z]{1,4}$`: в `--verify` их вообще снимало с проверки, а
   в полном режиме отдавало позиционному выравниванию, которое вставку поля не
   видит (`norm()` сводит все обфусцированные имена типов к одному токену
   `OBF_o`, последовательность типов становится неразличимой). Теперь короткое
   имя считается мусорным, только если регистр не похож на слово: `uRP`, `ccW`,
   `nfD` — да; `Aim`, `KnockDoor`, `m_Text` — нет. Заодно в `--verify`
   обфусцированные поля больше не пропускаются целиком: сверяются смещение+тип.
   Число непроверяемых в `--verify` констант упало с 42 до 20.

Проверка (обе ловят ошибку заново, если значение вернуть):

    python3 tools/offsets/update_offsets.py --verify --no-rva
    # EVENT_HANDLER_AIM_ACTIVITY   было 0x268   стало 0x270
    #   источник: Gum_Fields via playerEventHandler.Aim (имя)

В полном режиме (со старым дампом) `align()` теперь ведёт поля хендлера по
именам: `Aim` 0x268→0x270, `InteractContinuously` 0x188→0x190, `Voice`
0x270→0x278 — то есть следующий апдейт этот сдвиг увидит сам.

**Что перепроверено вручную заодно** (всё цело, правок не потребовало):
`manager` 0xD0 и `LookDirection` 0x140 не двигались; флаг активности 0x10
(класс активности `Dqg`→`Gub`, раскладка из 6 полей та же);
`PLAYER_EVENT_HANDLER` 0x78; голосовая цепочка `voicePlayer` 0x140 → тег 0x78
(класс `DEY`→`Gmv`, имя поля `uRP`→`ccW`, тип `System_String_o*` прежний) →
`VoicePlayerState` 0x2E8 → `_Name_k__BackingField` 0x38; никлейбл 0x130 →
`player` 0x20 → `nickname` 0x38 → `m_Text` 0xE0. Класс weapon view (`en`→`bp`)
в обоих билдах не имеет собственных полей — `WEAPONVIEW_*` остаются
рантайм-значениями из базового класса, менять нечего (сдвинулся только сам
указатель `PLAYERWEAPON_VIEW` 0xD0→0xE0, это сделано в `49e6dc9`).

### 13 сентября 2026: дальность удара по руде/дереву — из дампа, а не «на глаз»

Запрос: определить по дампу максимальную дальность удара по руде/дереву и т.п.
Подробности с RVA и дизасмом — в §3.11, здесь только итог и что изменилось.

Что нашлось:

* дальность удара — это **`FPMelee.m_MaxReach` (0x128) + `FPMelee.hitRadius`
  (0x12C)**: ровно эту сумму `FPMelee.ZkX` сравнивает с `RaycastHit.distance`,
  и если не дотянули — играет `On_Woosh()` вместо `On_Hit()`;
* `distance` — 3D-метры **от камеры/оси выстрела**, а не горизонтальное
  расстояние до узла. Автофарм до этого мерил по горизонтали (`aim_dist`)
  эмпирическими порогами 0.80 м (дерево) и 1.55 м (руда), подобранными на
  устройстве;
* самих чисел в дампе нет — они сериализованы в префабе каждого инструмента. В
  конструкторах заглушки: `m_MaxReach 0.5`, `hitRadius 0.1` (плюс
  `m_TimeBetweenAttacks 0.85`, `m_DamagePerHit 15`, `m_ImpactForce 15`),
  `m_RayLength 1.5`, `m_AimRayLength 1.5`, `m_TooCloseThreeshold 1.0`. Поэтому
  значение читается из живого орудия в руках;
* основной луч (`RaycastData`, активность 0x160) кастуется на `m_RayLength`, а
  запасная сфера (`AimRaycast`, 0x168) — только если основной промахнулся; её
  длину и радиус `FPMelee.On_Draw` заполняет теми же `m_MaxReach`/`hitRadius`.
  Отсюда рабочая дальность — `m_MaxReach + hitRadius` от глаза.

Что сделано:

* `game_offsets.h`: восемь новых констант (`FPMELEE_MAX_REACH`,
  `FPMELEE_HIT_RADIUS`, `FPOBJECT_RAYCAST_MANAGER`, `RAYCASTMAN_PLAYER`,
  `RAYCASTMAN_RAY_LENGTH`, `RAYCASTMAN_AIM_RAY_LENGTH`,
  `RAYCASTMAN_SPHERE_RADIUS`, `RAYCASTMAN_TOO_CLOSE`) с provenance в
  комментариях; все занесены в `offsets_map.json` (стало 162 записи),
  `update_offsets.py --verify` проходит без замечаний — 122 полевые константы
  сверяются, из них новые по читаемым именам;
* `game.cpp`: `read_local_melee_reach()` — локальный игрок → `FPManager` →
  текущее оружие → back-ref → **имя класса** (только семейство `FPMelee`:
  `FPTool`, `FPChainsaw`, `FPMelee`, `FPSpear`, `FPBuildingHammer`, `FPTorch`) →
  чтение двух float с проверкой правдоподобия; имя класса кэшируется на
  `Il2CppClass`, чтобы не дёргать строку каждый кадр. Без проверки имени читать
  0x128 нельзя: у `FPCrossbow`/`FPSnowball` там `m_MaxDistance` — сотни метров.
  Наружу значение отдаётся полем `FarmTarget.melee_reach` (отдельного API не
  заводим — единственный потребитель сейчас автофарм);
* `game.h` + `game.cpp`: `FarmTarget.aim_3d` теперь считается от глаза/оси
  выстрела — той же точки, от которой берутся углы наведения, — а не от
  `g_frame_local_pos` (корень игрока, ~1.5 м ниже глаза). Раньше это поле никто
  не читал, и оно расходилось с собственным описанием; добавлены `melee_reach` и
  `melee_ray`;
* `main.cpp`: если дальность орудия прочитана, порог «можно бить» и «поджимать
  вперёд» считается по правилу игры в `aim_3d` (запас 0.90, гистерезис 0.98, по
  корпусу без крестика +1.20 м на радиус узла). Если не прочитана — прежние
  эмпирические пороги по горизонтали, поведение не меняется. В панели цели живая
  дальность видна: «Дерево · 1.9 м · удар до 2.40» (или «удар ?», если орудие не
  опознано).

Проверено: `game.cpp` компилируется (`g++ -fsyntax-only -std=c++17`), изменённые
выражения `main.cpp` — изолированным тестом с теми же константами и мок-переменными
(NDK в песочнице нет, полный билд делает CI). Три сценария дали ожидаемые пороги:
дерево с живой дальностью 2.40 → 2.16 м по `aim_3d`; без живой → прежние 0.80 м
по горизонтали; руда по корпусу с живой 2.10 → 3.09 м и стик продолжает поджимать.

### 13 сентября 2026: автофарм добрал всё из дампа + починено мерцание боксов

Две задачи одним заходом: (1) «дополни автофарм всем, чем можно из дампа»,
(2) «фиксани мерцания визуалов — иногда мерцают и телепаются в другую сторону».

**Автофарм (всё из дампа `62a8534`, подробности и дизасм — §3.12):**

* `game_offsets.h`: `MINEABLE_REQUIRED_TOOL_PURPOSE` (0x70),
  `MINEABLE_EXPERIENCE` (0xD4), `FPMELEE_TIME_BETWEEN_ATTACKS` (0x130),
  `FPMELEE_PAUSE_AFTER_ATTACK` (0x134), `FPTOOL_TOOL_PURPOSES` (0x160),
  `FPOBJECT_EVENT_HANDLER` (0xC8), `GUM_RAYCAST_DATA` (0x160),
  `GUM_AIM_RAYCAST` (0x168), `GUI_VALUE` (0x20), `GKO_HIT_OBJECT` (0x18),
  `GKO_RAYCAST_HIT` (0x48), `RAYCASTHIT_DISTANCE` (0x1C), `HITMARK_AGE` (0xC8);
  `enum class ToolPurpose` (CutWood=1/BreakRocks=2/CutAnimals=4). Мёртвые
  прежде `OREMARK_AGE`, `OREMARK_LIFETIME`, `HITMARK_LIFETIME`,
  `MINEABLE_CURRENT_HEALTH`, `MINEABLE_MAX_HEALTH`, `TREEHS_SPOT_B` теперь
  работают; `OREMARK_SCALE` переименован в `OREMARK_SMOOTH` (0x48 — это `lzA`,
  плавное значение, а не масштаб). Новые константы не заводились «про запас»:
  `m_Resistance`, `m_Efficiency`, `lzT`/`ignoringLayerMask`/`sizeByDistance`
  упомянуты только в комментариях и в карте расклада;
* `game.cpp`: узел, который нечем взять текущим орудием, целью не становится
  (`m_RequiredToolPurpose` читается один раз при скане, маска орудия — из того
  же объекта, что и дальность удара, и только для `FPTool`/`FPChainsaw`).
  Причина простоя 6 — «нужен топор/кирка» — видна в меню вместо «все узлы вне
  радиуса»;
* `game.cpp`: крестик считается живым только пока не истёк его собственный
  таймер (руда — `lHG` против 15.0, дерево — `lzr` против своего `lifetime`).
  Потухший X больше не уводит прицел в невидимую точку: бот бьёт по корпусу, и
  игра создаёт крестик заново. Остаток жизни показан в статусе («есть · кора ·
  3 · 7 с»);
* `game.cpp`: у дерева прицел ставится в ближайшую к глазу точку отрезка
  `MTQ..MTu` (игра меряет попадание до отрезка, радиус 0.15 м), но только когда
  она строго внутри отрезка и ближе конца A на 15+ см;
* `game.cpp`: перекрытие узла видно по лучу самой игры
  (`Gum.RaycastData/AimRaycast -> GKo.RaycastHit.m_Distance`); `main.cpp` в этом
  случае не тапает и уходит манёвром обхода, а после исчерпания попыток сдаёт
  узел. Фаза показывается как «перекрыт — обхожу»;
* `main.cpp`: ритм тапов берётся из `m_TimeBetweenAttacks + pauseAfterAttack`
  орудия (на 2% медленнее кулдауна), а не из подобранных 85/230 мс;
* `main.cpp`: watchdog видит прогресс по `m_CurrentHealth` — здоровье падает от
  первого дошедшего удара, тогда как `fractionRemaining` сдвигается на проценты
  спустя десятки попаданий. Give-up таймер теперь считается надёжным, если
  читается здоровье ИЛИ остаток;
* `game.h`: `FarmTarget` дополнен полями `tool_purposes`, `attack_period`,
  `node_health`, `node_health_max`, `node_experience`, `ray_valid`,
  `ray_distance`, `ray_blocked`, `spot_life`; новый `esp_farm_tool_info()`;
* `main.cpp`: в окне автофарма появилась строка «Орудие» (что умеет + ритм +
  опыт за узел), в «Цель» добавлено `HP %`.

**Мерцание и «телепорт» боксов** — нашлись четыре независимые причины, все
починены (`game.cpp`):

1. `track_player()` перезаписывал кеш `userID` результатом чтения
   **безусловно**: любой сбой чтения стирал uid, а без uid не работает
   подавление дублей объекта (копия после посадки в транспорт, остаток
   респауна) — второй бокс вспыхивал в стороне и пропадал. Теперь кеш
   обновляется только успешным чтением (как уже сделано для имени/оружия),
   а при неудаче без кеша повторная попытка через 20 кадров вместо 120;
2. позиция принималась любой конечной: нули после респауна, денормали и
   координаты чужого объекта из пула проходят `vec3_is_finite()`. Добавлена
   проверка `position_looks_like_world_space()` — тот же фильтр, что уже
   использовался в `apply_mounted_position()`;
3. одиночный сбой чтения гасил бокс на кадр, а одиночный мусорный отсчёт
   утаскивал его в сторону. Добавлен временной фильтр
   `filter_player_position()`: до 3 кадров бокс дорисовывается по последней
   принятой позиции (продолженной по измеренной скорости, не дальше 1.5 м), а
   скачок за пределами допуска `3 м + 55 м/с * dt` рисуется только когда
   подтверждается 2 кадра подряд. Настоящий телепорт/респаун принимается через
   ~33 мс, скорость после него сбрасывается;
4. список игроков читается по одному указателю на элемент, и сбой одного
   чтения выбрасывал игрока из кадра. Пропавший держится в списке ещё 2 кадра
   (только если население совпадает со старым — на перезагрузке мира
   `reset_world_caches()` по-прежнему срабатывает сразу).

Проверено: `game.cpp` компилируется (`g++ -fsyntax-only -std=c++17`); логика
фильтра, прицела в отрезок, ритма тапов, фильтра инструмента и живости
крестика — изолированными тестами на копиях кода из репозитория (NDK в
песочнице нет, полный билд делает CI). Фильтр позиции: ходьба 5 м/с и техника
45 м/с — расхождение с истиной 0.000 м; мусорный кадр на +40 м — бокс сместился
на 0.100 м вместо ~47 м; сбой чтения — 3 кадра удержания, затем гаснет;
телепорт на 30 м — принят на 2-м кадре; обновления пачкой 10 Гц — без
отставания. `update_offsets.py --verify --no-rva`: 134 полевые константы + 3
RVA, изменений нет; `offsets_map.json` — 175 записей, `PROVENANCE.md`
пересобран `make_provenance.py`.

### 14 сентября 2026: покадровый лог автофарма в Загрузки

Жалоба: «много ошибок в движениях и т.п.» — по одному только экрану не
разобрать, какое звено врёт. Сделан постоянный лог автофарма (§6.1): строка на
кадр с 44 столбцами + строки `EV` на каждое решение контроллера, файл
`Загрузки/farm_debug.log` с ротацией на 8 МБ и тумблером «Отладка → Лог фарма»
в окне автофарма (по умолчанию включён; в конфиг не сохраняется — байтовый
расклад `CfgBlob` версии 4 заморожен, ломать совместимость ради отладочного
флага не за что).

Что нового читается из игры ради лога: `esp_local_eye_position()` — позиция
глаза локального игрока (точка выстрела KCC `g_aim_ref_origin`, иначе поза
камеры `g_cam_pos`). По её производной лог показывает, куда и как быстро
персонаж пошёл НА САМОМ ДЕЛЕ (`mv_dps`/`mv_dir`, направление приведено к
камере), и это можно сравнивать с командой стику (`slen`/`sdir`) напрямую:
расхождение — зона джойстика выставлена мимо или раскладка не та, `mv_dps ~ 0`
при отклонённом стике — упёрся в геометрию.

В шапку файла пишутся зоны бота (джойстик/огонь в процентах экрана и
«калибровано/по умолчанию»), размер экрана, радиус поиска и маска ресурсов:
половина «ошибок движения» — это некалиброванный джойстик, и в самих строках
такого не видно.

**Баг, который лог и вскрыл.** Стенд (`/tmp/ftest/ctrl_test.cpp` — настоящий
код `constants + farmlog + UpdateFarm/UpdateFarmInner` из `main.cpp` вокруг
заглушек окружения) на сценарии «перекрытый узел» показал бесконечную серию
`обход #1 (вправо)`: watchdog в фазе 3 каждый кадр делал `s_evadeCount = 0;
s_evadeTime = 0.f`, то есть обрывал манёвр, начатый в этой же фазе из-за
`ray_blocked`. Обход не отрабатывал ни кадра, счётчик не рос, и до отказа от
узла (`kEvadeMax`) дело не доходило — бот стоял и молотил в перекрытие, пока
его не сдавал watchdog «нет прогресса» через 20 с. Добавлено `s_evadeWhy`
(0 нет / 1 застрял на подходе / 2 узел перекрыт): в фазе 3 бюджет обходов
сбрасывается только если манёвр начат не перекрытием, а после того как узел
снова простреливается и манёвр доделан, счётчик прощается — иначе следующая
перекрытая точка начиналась бы сразу с отказа. Прогресс-watchdog при этом
работает в обоих случаях (проверено: `#1 → #2 → #3 → #4 → blacklist`).

Проверено без NDK: `game.cpp` — `g++ -fsyntax-only`; контроллер и лог — тем же
стендом с `-Wall -Wextra -Wformat=2` (`event()` помечен
`__attribute__((format(printf,1,2)))`, поэтому все 16 мест вызова проверены
компилятором по типам и числу аргументов). Прогон сценариев: подход и добыча
узла, кривой ответ камеры (сброс коэффициента) и цель далеко в стороне (палец
камеры уезжает в край), перекрытый узел и четыре обхода, мёртвый узел (20 с без
прогресса → отказ), пропажа цели надолго (`reason`), выключенный фарм; отдельно
— ротация на 8 МБ (40 000 строк: `farm_debug.1.log` 8.4 МБ + новый файл с
новой шапкой, ~4.5 мкс на строку) и реальное время для `mv_dps` (при 60 к/с
`sdir 8.1°` = `mv_dir 8.1°`, скорость растёт вместе с отклонением стика).
`main.cpp` целиком компилирует CI.

### 14 сентября 2026: «бьёт в воздух» — прицел стоял на декали крестика, а не на коре

**Жалоба.** «автофарм бьёт в воздух — крестик появляется сбоку дерева, а бот
бьёт по зелёной метке автофарма, которая отступает от коры дерева, от этого
удары в воздух». То есть глазами видно: зелёная метка бота висит НЕ на стволе.

**Разбор `farm_debug.log`** (14 500 строк, ~4 мин, 87 замахов; урон считался по
падению `hp` того же узла в окне 1 с после фронта `tp` 0→1):

| | урон есть (39) | в воздух (48) |
|---|---|---|
| `dist` / `aim3d`, м | **0.51** | **0.80** |
| луч игры читается (`ray` ≠ -99) | **37/39 (95%)** | **5/48 (10%)** |
| ошибка камеры \|yaw\| / \|pitch\| | 0.1° / 0.3° | 0.1° / 0.5° |
| `at`=1 (прицел на крестике) | 33/39 | 44/48 |

Камера наведена точно в обоих случаях — значит промах не от наведения. Решает
наличие у игры данных рейкаста: `FPMelee.ZkX` берёт `distance` из обёртки `GKo`
(`RaycastData`/`AimRaycast`), а с пустой обёрткой удар не засчитывается вовсе,
играет один `On_Woosh`. По кадрам фазы 3: валидность луча **97% при `aim3d`
≈0.50 м и 19% при ≈0.75 м** — бот стоял в основном на 0.75 м.

**Причина.** Точка прицела бралась из трансформа декаля: `spot`=3 во всех
10 887 строках и ни разу 2 (`TREEHS_SPOT_A`, точка на коре). А декаль, как
записано ещё в §3.10 по `HitMarkerItem.giI`, ставится в
`точка_на_коре + normal * 0.25` — то есть висит в 25 см ОТ коры. Когда крест на
боку ствола, луч «глаз → декаль» проходит мимо дерева: чем дальше стоишь, тем
меньше ствол перекрывает этот уход вбок (отсюда 97% → 19%). Заодно точка
подхода (`spot + 0.45 м наружу`) отмерялась от декали, поэтому бот вставал на
0.70 м от коры — ровно там, где луч уже мёртв.

**Заодно — простой на мигании цели.** Узел `b31b40` (20 м, hp 100%) перехватывал
цель ровно на один кадр 9 раз за сессию (видимо, на рескане реестра рабочая цель
на кадр выпадала из списка), и каждая смена в обе стороны ставила
`s_settle = kSettleTime` = 0.7 с: **854 кадра с `ex`=6, то есть ~14 с из 150 бот
просто стоял с поднятыми пальцами.**

**Что исправлено.**
1. `esp_farm_get_target()`: точка крестика из источника 3 (декаль) у дерева
   тянется к оси ствола (пивот узла — в центре ствола) ровно на 0.25 м, то есть
   встаёт на кору. Тянем ПОСЛЕ проверки «с нашей ли стороны X» — там декаль
   различимее. Точка подхода при этом сама становится на 0.25 м ближе.
2. Контроллер: тап держится, пока у игры нет луча прицела (`noRay`), но не
   дольше `kNoRaySwingAfter` = 2.5 с (вдруг на другой сборке обёртка просто не
   читается); всё это время стик поджимает бота ближе — до
   `kNoRayCloseTo` = 0.42 м, где луч живёт.
3. `esp_camera_angles()` / `esp_local_eye_position()`: добавлен третий
   источник — базис и позиция из матрицы вида этого кадра. В логе `gain` стоял
   на 0.250 (константа-догадка `kCamGainProbe`) во ВСЕХ 11 932 кадрах, а
   `mv_dps`/`mv_dir` были -99 во всех 14 499 строках: на этом устройстве поза
   камеры и ось выстрела не читаются вовсе, и камера с движением работали
   разомкнуто. Фарм при этом считал yaw/pitch именно из базиса — он и отдан.
4. Фильтр мусорного источника камеры (`farm_cam_source_ok`): позиция обязана
   быть конечной и в пределах 25 м от корня игрока. В логе было 11 кадров с
   `origin=(0,0,0)` — `aim3d` 864 и 1027 м при `dist` 0.8 м и yaw 158-164°,
   то есть камера получала команду развернуться почти на пол-оборота. Нулевой
   вектор конечен, поэтому прежняя проверка `isfinite` его пропускала.
5. Диагностика, чтобы следующий лог был самодостаточным: читаются
   `RaycastHit.m_Point` (0x0) и `m_Normal` (0xC) — раскладка подтверждена по
   `dump.cs` билда `62a8534`, строка 642061; в `FarmTarget` добавлены точка
   прицела и пивот узла в мировых координатах; `esp_farm_spot_raw()` отдаёт
   сырые `SPOT_A`/`SPOT_B`, `|A-B|` и причину, по которой `SPOT_A` не стал
   прицелом (0 принят / 1 не конечен / 2 вне узла / 3 не читали). Всё это
   печатается в новых строках `EV замах:` и в расширенной `EV крестик появился:`
   (формат 44 колонок не менялся).

6. Дебаунс смены цели: новый узел обязан продержаться «лучшим»
   `kNodeDebounce` = 3 кадра ПОДРЯД (если цель вернулась — кандидат
   сбрасывается, иначе редкие мигания накапливались бы и переключение всё
   равно случалось — поймано стендом). Однокадровое мигание отрабатывает как
   «цель мигнула» (`ex` 4): ввод заморожен, прежний узел не сброшен, в лог
   идёт `EV цель мигнула на узел <id> (dist/aim3d) — держим <id>`. Если цель
   прыгает дольше `kFlickerGiveUp` = 1 с, переключаемся по-настоящему — иначе
   бот застыл бы навсегда.

**Проверено без NDK.** `game.cpp` — `g++ -fsyntax-only -Wall -Wextra`
(единственное предупреждение — прежний multi-line comment в `game_offsets.h`).
Контроллер и лог — стенд `sh tools/farm/run.sh`, два новых участка: **F** (2 с с
живым крестиком и севшим прицелом, но без луча игры) — `тапов 0`, стик жал все
120 кадров, после появления луча `тапов 6`; **G** (5 однокадровых миганий цели)
— `удержано 5`, `пауз_смены_после_мигания 0`. Проверки в `run.sh` фиксируют оба.
`main.cpp` целиком компилирует CI.

**Что смотреть в следующем логе.** (1) `ray` ≠ -99 в фазе 3 — должно быть почти
всегда; (2) `gain` — должен уйти с 0.250 и появиться `EV камера: коэффициент`;
(3) `mv_dps`/`mv_dir` — должны ожить; (4) в `EV замах:` — `луч <м> (±м к
прицелу)`: расхождение точки луча и прицела покажет, точно ли угадан сдвиг
декали 0.25 м; (5) в `EV крестик появился:` — `why`: если 1/2, то `SPOT_A` в
памяти есть, но бракуется, и тогда точку на коре можно брать напрямую, без
поправки; (6) `strk` — должен расти сериями, а не сбрасываться после каждого
попадания.

### 14 сентября 2026: «прицел дёргается» и «крестик проваливается в кору»

**Жалоба.** «Прицел дергается, и когда бот начинает бить дерево, крестик
появляется сбоку — бот начинает мазать по нему, потому что крестик теперь
проваливается в кору». Разбирался новый `farm_debug.log` (коммит `47a41b8`,
6.5 МБ, 25 997 строк, t = 74.87..188.70, 821 событие). Причины две, и они
независимые: первая — регрессия прошлого коммита, вторая — ошибка в самой
поправке на кору.

**1. Прицел дёргается: обучатель коэффициента камеры мерил одним кадром.**

Прошлая правка (`6b0de64`) отдала фарму позу камеры из матрицы вида — `camYaw`
ожил, и вместе с ним заработал обучатель коэффициента, который делил поворот
одного кадра на сдвиг пальца одного кадра. Камера так не работает:

* отклик приходит через **2 кадра** (медиана; p90 4, макс 6) при кадре 8.5 мс
  (118 к/с);
* пока отклика нет, контроллер досылал ещё **9..11 шагов подряд** (медиана 9,
  p90 11) — шаги складывались, камера перелетала крестик, знак ошибки менялся и
  палец швырял её обратно;
* обучатель на таких кадрах сбрасывал коэффициент **571 раз за 122 с**, значения
  гуляли `-1.678..1.972` (509 различных), а медиана осталась запасной `0.2500`
  в 11 946 кадрах. Один и тот же свайп −81 px измерялся как −2.39°, −13.21° и
  −17.02°;
* при `0.0083` шаг на ошибку 1° выходил 120 px (рывок 8° за кадр), при `1.97`
  палец двигался, а камера стояла.

Настоящий коэффициент, посчитанный по накопленному сдвигу за свайп (свайп
закрывался после 15..60 кадров покоя, без нажатого джойстика): **0.1031 град/px**
по yaw (35 образцов, p10 0.078, p90 0.178) и ~0.06 по pitch. Он не зависит ни от
устойки (3/10/20/40/60 кадров дают 0.087..0.100), ни от скорости свайпа
(корреляция с `max|ldx|` 0.09) — то есть это честная константа устройства, а не
шум. Запасное 0.25 было в 2.5 раза больше: палец слал в 2.5 раза меньше
пикселей, чем нужно.

Сделано в `main.cpp`:

1. **Такт с подтверждением** (приём уже работал в аимботном пальце, в фермерском
   его не было): шаг послали — держим палец на месте и ждём отклика камеры
   (`kCamMovedEps` 0.05°, таймаут `kLookWaitMax` 0.10 с) — только потом следующий
   шаг. Вслепую шаги больше не копятся.
2. **Коэффициент по накопленному сдвигу** между двумя откликами камеры
   (`s_gainPendDx`), а не по одному кадру. Образец принимается при |сдвиг| ≥
   `kGainMinPx` 12 px, том же знаке (кроме первого образца — так ловится
   инверсия оси в настройках игры) и в полосе `kGainLo..kGainHi` 0.035..0.350.
   Обновление — медиана последних трёх образцов и ЭМА 0.4, событие в лог только
   при изменении больше 10%. Полосу держим и на запасном значении, знак
   сохраняем.
3. Запасное `kCamGainProbe` 0.25 → **0.10** (измеренное).
4. Предел шага по крестику `kStepSpotFrac` 0.030 высоты экрана вместо 0.075:
   81 px при 0.10 град/px — это 8° рывком, даже когда коэффициент верный. На
   подходе прежние 0.075 оставлены (быстрый доворот нужнее плавности).
5. **Квант ввода** (`kStepQuantumPx` 1 px): позиция пальца пишется в
   цифрователь целыми, поэтому шаг меньше пикселя до камеры не доходит вовсе.
   Ошибка, на которую шаг меньше кванта, считается севшей и касание
   отпускается — иначе палец вечно держался опущенным и слал по 0.8 px за кадр
   (стенд ловил 12 кадров подряд на ошибке 0.5°).
6. Мёртвая зона по pitch в упор `kPitchDeadSpot` 0.50° вместо 0: в логе ошибка
   по pitch держалась 0.4..0.7° в 57% кадров доводки, и палец не отпускал
   касание. Полградуса на 0.8 м — это 7 мм, в разы меньше засчитываемой зоны X.

**2. Крестик проваливался в кору: `SPOT_A`/`SPOT_B` оказались локальными.**

`TREEHS_SPOT_A` (MTQ, 0x88) и `TREEHS_SPOT_B` (MTu, 0xA4) — **локальные**
координаты дерева, а не мировые. Во всех 6 строках `EV крестик появился`
(`why` 2) A = (0, h, 0) — точка на ОСИ ствола на высоте X, а B отстоит от неё на
радиус ствола в локальных единицах: 0.40 у дерева с корой 0.354 м и 0.04 у
тонкого с корой 0.037 м. Отношения (0.885 и 0.93) сходятся с масштабом дерева по
высоте (1.93 → 1.74). Мировой точкой A быть не может — узел в 1200 м от начала
координат, поэтому прежняя ветка «прицел = A» (источник 2) была мертва и могла
сработать только у деревьев рядом с (0,0,0), давая там мусор; удалена.

Прошлая поправка тянула прицел к оси на 0.25 м («сдвиг декали из дампа»). Замер
по 55 замахам с живым лучом игры (известны и `m_Point`, и `m_Normal`): декаль
торчит из коры в среднем на **0.108 м** (0.00..0.31), а прицел после поправки
уходил на **0.138 м ВНУТРЬ ствола** (min −0.178, max +0.063 снаружи). По высоте
всё сходилось (±0.07 м) — ошибка была чисто по нормали.

Почему от этого мажет: игра меряет расстояние от точки попадания до отрезка
A..B радиусом 0.15 м.

| куда поставлен прицел | вид вдоль декали | вид перпендикулярно декали |
|---|---|---|
| на декали (+0.108 м снаружи) | зачёт | луч вообще не пересекает ствол — удар в воздух |
| на 0.138 м внутри ствола | зачёт | 0.32 м от отрезка — промах |
| **на коре (радиус ствола)** | **0 м** | **0.13 м — зачёт** |

Кора — единственная точка, которая попадает в допуск при любом угле подхода.
Урон при этом шёл (40 попаданий / 23 промаха на одном узле), но серия выросла
только на 19: попадания ложились примерно в 15 см от X, то есть на границе
допуска.

Сделано в `game.cpp`: прицел ставится на пересечение направления на декаль с
корой, радиус ствола на высоте X берётся по убыванию точности —

1. точка попадания луча самой игры `RaycastHit.m_Point`, с проверками что
   попадание на этом стволе (0.03 < r < 2.5 м, r < 1.6×горизонтали декали,
   |Δy| < 1.2 м) — иначе это земля, соседнее дерево или крона;
2. локальный отрезок A..B × масштаб дерева, масштаб = высота декали над пивотом
   / A.y (guard'ы: A.y > 0.25, 0.15 < scale < 6, r_local > 0.005);
3. совсем ничего не прочиталось — 0.77×горизонтали декали (декаль торчит примерно
   на 0.3 радиуса; замер 0.757..0.788 на двух деревьях).

Радиус сглаживается на узел (ЭМА 0.25, одна ячейка `s_bark_id`/`s_bark_r` —
узел один за раз): луч игры попадает в разные места коры и шумит на сантиметры,
без сглаживания прицел ползал бы вместе с шумом, а точка подхода дёргалась за
ним. Направление всегда от декали: нормаль ствола почти радиальна, а величина
сдвига по нормали нам как раз неизвестна — она и есть ошибка.

**Проверено без NDK.** `game.cpp` — `g++ -fsyntax-only`. Контроллер — стенд
`sh tools/farm/run.sh`, у которого переписана модель камеры: теперь она отвечает
на сдвиг пальца 1 через `g_camLag` = 2 кадра с коэффициентом 0.14 град/px
(намеренно не равен запасному 0.10 — иначе по логу не отличить «выучил» от «так
и сидит на запасном»), а ошибка наведения считается от позы камеры, поэтому
доводка обязана сходиться к нулю. Добавлен участок **H**: 4200 кадров, 37 шагов
пальца, **стопок 0** (максимум 1 шаг подряд), **перелётов 0**, **смен знака 0**,
коэффициент 0.1000 → 0.1400 (сошёлся к модельному) и в окне B, где
чувствительность 10 с держится на 0.30, пошёл за ней до 0.2793, не вылезая за
полосу. Все четыре числа зафиксированы проверками в `run.sh`. Попутно стенд
поймал две ошибки самой правки: условие «не учиться на ходу» отрезало ВСЕ
образцы (джойстик нажат почти всегда, а поворот камеры от ходьбы не зависит) и
субпиксельные шаги не попадали под ожидание отклика (отсюда квант ввода).

**Что смотреть в следующем логе.** (1) `gain` — должен уйти с 0.100 и держаться
около 0.10; событий `EV камера: коэффициент` единицы, а не сотни. (2) Между
двумя ненулевыми `ldx` обязаны появляться нулевые строки — это ожидание отклика;
подряд идущих ненулевых `ldx` быть не должно, это и есть стопка шагов.
(3) В `EV замах:` расхождение `луч` и `прицел` должно упасть до 0.00..0.03 м
вместо −0.14. (4) `strk` должен расти сериями, а `yaw`/`pitch` в фазе 3 —
держаться около 0.5° без пилы. (5) `why` в `EV крестик появился:` по-прежнему 2
— это нормально: локальные значения идут на радиус ствола, а не на прицел.

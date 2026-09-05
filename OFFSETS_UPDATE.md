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
| `PLAYER_MANAGER_TYPEINFO_RVA` | 0xD7E4310 | Oxide.PlayerManager |
| `GAME_CONTROLLER_TYPEINFO_RVA` | 0xD7DF6C8 | GameControllerBase |
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
6. Пересобери: пушишь в `arena/01a068cd-myproject1` → GitHub Actions соберёт
   `xvcen-sh-arm64-v8a` → забираешь артефакт из CI.
7. Проверка на устройстве: ESP показывает **реальные ники**, корректное оружие,
   боксы не сливаются. Если что-то не так — верни временный дамп полей из
   истории коммитов, таблица в разделе 6.

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

Все три — одноразовые (пишут при первом скане/первых 8 игроках), вызов ставится
рядом с местом, где значение уже посчитано.

## 7. Журнал апдейтов игры

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

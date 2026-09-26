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
- **`jni/src/main.cpp`** — только точка входа: инициализация, главный цикл, состояния приложения (`app_state.h`). Монстр-файл разбит на модули ниже; список исходников — в `jni/Android.mk`.
- **`jni/src/game.cpp`** — ядро чтения памяти: привязка к процессу, камера, игроки, скелеты, кадр (позиции/вращение для ESP), melee-дальность орудия.
- **`jni/src/game_markers.cpp`** — маркеры мира для ESP (рудные узлы, деревья, животные, лут, пикапы, бочки): реестр `Mirror.NetworkClient.spawned`, периодический скан, позиции по кадру.
- **`jni/src/game_farm.cpp`** — игровой слой автофарма: скан реестра добываемых узлов (свой от маркеров — фарму нужны деревья), кэш сущностей, ре-скан, чёрный список.
- **`jni/src/game_internal.h`** — общий срез трёх `game*.cpp`: общее состояние кадра/игрока, обёртки чтения `rd`/`wr`, структуры (`TransformHierarchyLayout`, `MeleeReach`, `FarmEntity`, `MarkerClass`), резолверы реестра.
- **`jni/src/farm.cpp`** — контроллер автофарма по кадрам: поворот/подход/удар синтетическими тачами, прицел по крестику (hitstreak), доводка камеры; статусные `g_farm*` для строки в меню.
- **`jni/src/esp_draw.cpp`** — ESP-оверлей: один снимок боксов/маркеров на кадр (`FrameBoxes()`, шарят аимбот и автофарм) и отрисовка.
- **`jni/src/aim.cpp`** — аимбот: ведение камеры синтетическим «пальцем взгляда», коэффициент град/px из чувствительности клиента.
- **`jni/src/process.cpp`** — поиск процесса игры и фоновая привязка к нему.
- **`jni/src/menu.cpp` / `widgets.cpp` / `theme.cpp` / `hud.cpp`** — меню (вкладки, рейл с аватаром, калибровка), виджеты (скролл/окно/шторка/поповеры), тема (палитры/габариты), HUD (тосты, водяной знак).
- **`jni/src/config.cpp`** — конфиги `.cfg` в каталоге benzhack (12 слотов), язык, выбор версии релиз/бета, inotify каталога.
- **`jni/src/audio.cpp`** — звуки меню (OpenSLES); без `media/audio.h` — заглушки.
- **`jni/src/lang.cpp` / `jni/include/str.h`** — переводы РУ/EN и XOR-строки (`XS()`).
- **`jni/src/offset_resolver.cpp`** — автообновление оффсетов: по живому процессу находит классы по именам (замена `*_TYPEINFO_RVA`) и читает смещения полей из `FieldInfo`, перезаписывая активные слоты поверх вшитых таблиц (что не нашлось — остаётся на вшитом). Поля с ротированными именами ищет по ТИПУ. Результат первого входа кэшируется в `.offs` рядом с конфигами — дальше сканирование не нужно. Если вшитые RVA устарели, а имени `global-metadata` в карте процесса нет (после апдейта метадата мапится из `.apk`), блок метаданных ищется по сигнатуре `0xFAB11BAF` и окно имён берётся из его заголовка. Таблица запросов — `jni/src/offset_resolver_table.inc`, генератор — `tools/offsets/gen_resolver_table.py`. Стенды — `tools/offsets/resolver_test.sh` и `tools/offsets/metascan_test.sh`.
- **`jni/src/mem_io.h`** — доступ к памяти игры: два способа чтения (process_vm_readv и /proc/<pid>/mem), выбор пробой и автопереключение, кэш блоков. Стенд — `tools/memio/run.sh`.
- **`jni/include/game.h`** — публичный API слоя игры для остальных модулей; остальной API — в заголовках каждого модуля.
- **`jni/Android.mk` / `build.sh`** — сборка `libs/arm64-v8a/benzware.sh` (GitHub Actions собирает на push).
- Локальная проверка без NDK: `sh tools/hostcheck/run.sh` (все модули, хостовый g++) и `sh tools/syntax/check.sh` (по одному перевод-юниту на файл).

## 1.1. Как читается память игры (`jni/src/mem_io.h`)

Чтение и запись идут через один слой, и способ доступа там не один:

* `process_vm_readv` / `process_vm_writev` — быстрый путь, один syscall на пачку;
* `pread` / `pwrite` по `/proc/<pid>/mem` — второй путь, дескриптор открывается
  один раз и живёт до отвязки.

Способ выбирается пробой при привязке (читается ELF-заголовок по адресу начала
отображения `libil2cpp.so`) и автоматически меняется на второй, если выбранный
начал отказывать (8 отказов подряд). Причина не в стиле: политики прошивок режут
по-разному, и на части устройств запрещён именно `process_vm_readv` — привязка
«удавалась», меню рисовалось, а каждое чтение возвращало ошибку, то есть молча не
работала ни одна функция. Если не работает и второй способ, привязка честно
отказывает и в меню показывается причина («Нет доступа к памяти игры»).

Поверх — кэш блоков по 128 байт (8 блоков на поток): мелкие чтения полей внутри
кадра берутся из блока, а не из syscall'а. Кэш сбрасывается в начале кадра
(`esp_mem_frame_begin()` из главного цикла), а запись в память игры (Иксрей,
«Всегда день») помечает затронутый блок некэшируемым, поэтому своих же значений
из кэша не читаем. Замер стенда на профиле кадра ESP (40 объектов × 12 полей ×
8 кадров): 3840 syscall'ов → 320, время ×5.7.

Поиск процесса тоже другой: кандидат не «первый с совпавшим cmdline», а
проверенный по карте памяти (`libil2cpp.so` отображён); если такого нет — берётся
процесс-приложение (uid ≥ 10000) с отображённым `libil2cpp.so`, поэтому клон
приложения или сборка с другим applicationId тоже находятся. Пока привязка жива,
`/proc` не перебирается: раз в 1.5 с читается cmdline одного pid и одно чтение
памяти (`esp_alive_check()`), а перебор запускается только когда процесс ушёл или
доступ отобрали.

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
| `PLAYER_MANAGER_TYPEINFO_RVA` | 0xD9C5310 | Oxide.PlayerManager |
| `GAME_CONTROLLER_TYPEINFO_RVA` | 0xD9C0690 | GameControllerBase |
| `NETWORK_CLIENT_TYPEINFO_RVA` | 0xD9C4570 | Mirror.NetworkClient |
| `PLAYERLOOPHELPER_TYPEINFO_RVA` | 0xD9C52E0 | Cysharp.Threading.Tasks.PlayerLoopHelper (мгновенный подбор, §3.13) |
| `TOD_SCAN_RVA_BEGIN..END` | 0xD9B0000..0xDA50000 | окно скана TOD_Sky (не поле!) |

> Значения — для билда `72e26b9` (репо-снапшот 22 сентября 2026; отпечатки
> кандидатов и историю — см. `tools/offsets/PROVENANCE.md`, §B). Источник
> истины — `jni/src/game_offsets.h`; таблица здесь может отстать, заголовок
> не может.
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
| `PLAYER_VOICE_STATE` | 0x2F8 | VoicePlayerState (поле `Kmk`, ранее `uWr`/`LLT`) |
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

Цвета руд настраиваются по видам в карточке «Руды по видам» (умолчания:
камень — серый, железо — оранжевый, сера — жёлтая). Там же у каждого вида
свой тумблер: главный выключатель «Руда» в «Мире» гасит все маркеры разом,
а виды фильтруются поверх него при отрисовке (скан всегда собирает все руды,
поэтому переключение мгновенное и кэш не инвалидируется). Тумблеры видов
упакованы инверсными битами 64/128/256 в `esp_extra.x` (0 в старом конфиге —
все виды включены, поведение как раньше). Цвет животных настраивается.
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
   `benzware_esp_debug.log` — один и тот же `userID` на разных указателях).
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

**Запасной признак «свой» (состав команды, 24.09; runtime-цепочка расширена 25.09).**
Все три строки группы на чужих клиентах в наблюдавшейся сессии приходили пустыми,
поэтому добавлен независимый источник: командный объект локального игрока,
`PlayerManager.team` → `NetworkBehaviour` (`euD` в релизе, `Reo` в бете) →
`TeamData` (`wa` / `nX`). Сверка настоящих дампов: в релизе `wa_Fields` содержит
`aXG` (строка имени, `0x10`) и `aXU` (ссылочное поле списка, `0x20`); в бете
`nX_Fields` содержит `FLy` и `FLv` на тех же смещениях. У `TeamMember` в обеих
сборках `id/clanTag/nick/netId` лежат на `0x10/0x18/0x20/0x2C`.

Runtime-resolver теперь проходит живой путь `PlayerManager.team -> TeamComponent
-> TeamData` и покрывает `TEAM_OBJECT_PLAYER/DATA`, `TEAM_DATA_NAME/MEMBERS`,
`TeamMember.netId`, а также `NetworkBehaviour.netIdentity ->
NetworkIdentity.netId`. Для `TEAM_DATA_MEMBERS` применяется узкая сигнатура
`obj1`: сдвиг разрешается только при ровно одном ссылочном кандидате в целевом
классе; при нескольких кандидатах остаётся проверенный baked offset, без
угадывания. Ростер перечитывается раз в 30 кадров. Поле `TeamComponent.player`
не является обязательным gate при чтении списка; при прямом сравнении
компонентов валидная owner-ссылка используется только для отсева чужого
устаревшего объекта. Для instance-полей ссылочных объектов resolver и его кэш
отвергают смещения внутри заголовка объекта (`<0x10`): это не даёт некорректному
`FieldInfo` обнулить baked `_entries/_count` у `Dictionary`. Нулевые смещения
inline value-type полей (`RaycastHit`, `WeaponPiece`) остаются допустимыми.

Союзник подтверждается совпадением team/clan (с clan-tag fallback, если clan ID
отсутствует), общего `TeamData`/его имени, либо участника ростера по userID,
точному видимому нику, clan tag или Mirror `netId`. Признак `box.ally`
рассчитывается независимо от настройки показа: переключатель «Союзники» только
фильтрует их ESP, а аим всегда исключает подтверждённых союзников. Диагностика
`esp_get_ally_diag()` теперь указывает подтверждение локального PlayerManager,
доступность TeamData и этап/результат чтения ростера. В Android-сборке `LogLine`
сохраняется в `/storage/emulated/0/Download/benzware.log` (новый файл на запуск,
ограничение 2 МиБ); если Downloads недоступен, пробуются `/sdcard/Download`,
каталог конфигов и `/data/local/tmp`. Host-стенды по умолчанию не пишут файл.

### 3.7. Aим / события игрока (ADS)
| Константа | Смещение |
|---|---|
| `EVENT_HANDLER_MANAGER_BACKREF` | 0xD0 |
| `EVENT_HANDLER_AIM_ACTIVITY` | 0x290 |
| `EVENT_HANDLER_LOOK_DIRECTION` | 0x140 |
| `SYNC_VALUE_OFFSET` | 0x20 |
| `ACTIVITY_ACTIVE_FLAG` | 0x10 |

`EVENT_HANDLER_*` и `ACTIVITY_ACTIVE_FLAG` лежат НЕ в `PlayerManager`, а в
классе по указателю `playerEventHandler` (0x78): он ротирует имя каждый билд
(`DqO`→`Gum`→`brc`), и в его середину добавляют новые активности. Из-за вставленного
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
| `KCC_CHARACTER_ANIMATION` | 0xF0 |
| `KCC_LOOK_HEIGHT_OFFSET` | 0x90 |
| `KCC_MOVE` | 0x154 (структура Move, см. заголовок) |
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

### 3.13. Мгновенный подбор: UniTask-ожидания (лут из ящиков)

Задержка переноса предмета — это не игровой таймер, а ожидание
`Cysharp.Threading.Tasks.UniTask.DelayIgnoreTimeScalePromise` (вложенный в
`UniTask`, неймспейс **пустой**): все три потока переноса — кнопка «Собрать
всё» (`xw.CollectAllItems`), «быстрая загрузка» (`xv.FastLoadAll`) и
автоподбор зоны (`eDE.CollectAllItems`) — переносят по одному предмету за
`UniTask.WaitForSeconds(0.35f, ignoreTimeScale: true)`. Контроллер
(`jni/src/loot_memory.h`) обнуляет float-темп **только** у обещаний с
побитовым значением 0x3EB33333 (0.35f) и вразумительными `elapsed`/`initialFrame`
— чужие ожидания не трогаются. Единственная точка записи — `write_heap`
(тот же гейт, что у weapon_tuning); восстановление не нужно: игра пишет 0.35
заново при каждом новом ожидании.

**Диагностика конвейера (после `b505076`).** На устройстве наблюдалось
«ускоряет, но не мгновенно» при статусе Active и нулевых записях — статический
анализ всей цепочки (WaitForSeconds 0xc3290b4 → Delay → Create 0xc329abc:
span=(float)TimeSpan.TotalSeconds=0.35f@+0x18, elapsed=0@+0x1C,
initialFrame=frameCount@+0x20; завершается `bl AddAction 0xc322340` с
timing=Update) подтверждает каждую стадию, поэтому добавлены счётчики стадий
последнего тика (`Controller::Diag`): длина массива раннеров → непустые
слоты → опознанные раннеры → элементы `loopItems` → обещания нужного класса →
совпавший темп → прошедшие фильтры → записи. Отображаются строкой «Диаг: …»
в меню (при Active/Failed) и строкой `loot diag:` в логе (~раз в 2 с, при
изменении картины). Ноль в середине цепочки указывает стадию, на которой
теряются обещания.

Путь по памяти (классы UniTask не обфусцированы, сверяются по имени):

| Константа | Значение | Что это |
|---|---|---|
| `PLAYERLOOPHELPER_TYPEINFO_RVA` | 0xD9C52E0 | слот `Il2CppClass*` PlayerLoopHelper (§3.3) |
| `IL2CPP_CLASS_STATIC_FIELDS` | 0xB8 | `Il2CppClass.static_fields` |
| `PLAYERLOOPHELPER_RUNNERS` | 0x30 | `PlayerLoopRunner[] runners` (слот на каждый `PlayerLoopTiming`) |
| `LOOPRUNNER_ITEMS` | 0x38 | `IPlayerLoopItem[] loopItems` у раннера (длина = **ёмкость**) |
| `LOOPRUNNER_TAIL` | 0x30 | `int tail` — **живой счётчик**; живые элементы в `[0, tail)`, хвост после компакции — устаревшие указатели |
| `DELAYPROMISE_TIMESPAN` | 0x18 | `float delayFrameTimeSpan` — **сюда пишется 0.0** |
| `DELAYPROMISE_ELAPSED` | 0x1C | `float elapsed` |
| `DELAYPROMISE_INITIAL_FRAME` | 0x20 | `int initialFrame` |
| `DELAYPROMISE_SCALED_TIMESPAN` | 0x1C | `float delayTimeSpan` обычного (time-scaled) `DelayPromise` — **сюда пишется 0.0** |
| `DELAYPROMISE_SCALED_ELAPSED` | 0x20 | `float elapsed` (time-scaled вариант) |
| `DELAYPROMISE_SCALED_INITIAL_FRAME` | 0x18 | `int initialFrame` (time-scaled вариант) |

**Финальная картина (измерено на устройстве после `9d10af2`).** Обнуление
работает: ящик 15–30 предметов «Собрать всё» опустошает за 1–3 с (было
~0.35 с/предмет), рюкзак наполняется по одному — предмет за 1–2 кадра.
Оставшийся темп задаёт не сервер и не ожидание, а **сериальность самого
цикла**: один предмет → один await → минимум один игровой кадр (промис
создаётся во время прохода PlayerLoop и попадает в loopItems только
после его конца, а завершается не раньше следующего прохода). Это
структурный потолок heap-only: 30–60 предметов/с при 60 fps. Ручной
макрос по ячейкам быстрее на большом ящике, потому что отправляет все
запросы переносов пачкой, минуя цикл; повторить это без патча
инструкций нельзя. История наблюдений: на ранних билдах темп казался
неизменным (2–4 предм/с) из-за редких записей; после поддержки обоих
классов обещаний записи пошли стабильно (гистограмма: десятки
`00000000`-наблюдений за сессию). Полный дизасм цикла —
`xw.<CollectAllItems>d__6.MoveNext` 0x5b5c5bc..0x5b5cd30: проверки
контейнера → поиск предмета (JFP/фильтр Gsy) → PlayerInventory.FYj —
запрос переноса → await WaitForSeconds(0.35) → sourceIndex-- → следующий.

**Полный реестр создателей ignore-time-scale ожиданий (билд `72e26b9`, скан
BL-инструкций по .text).** Всего 134 call-сайта семейства Delay; с
`ignoreTimeScale=true` — 13: три потока подбора с литералом 0.35f
(`xw.CollectAllItems` 0x5b5cae4, `xv.FastLoadAll` 0x5b5d164,
`eDE.CollectAllItems` 0x6661778 — все `WaitForSeconds(0.35f, 1, Update)` c
литералом 0x213A950) и десять чужих (охота/туториал/сервер:
HuntMedalStack, HuntStagePresentation, HuntVictoryScreen×3,
PauseManager.BlinkSupportNotificationAsync — динамические значения;
IntroductionHuntScenario — Delay(250ms) и динамический;
hA.TryRefreshServerStatus и HY.WaitAsync — Delay(TimeSpan)). Цепочка
преобразования для лута пересчитана по инструкциям: 0.35f × 1000.0f (float,
без потери) → modf/round-half-even → 350 мс → `Interval`: (long)(350+0.5)
× 10⁴ = 3 500 000 тиков → `(float)TotalSeconds` = 0.35f **побитово**
(0x3EB33333) — т.е. лутовое обещание обязано иметь именно эти биты.

**Поддержка обоих классов обещаний + накопительная гистограмма (после
`5618b16`).** Диагностика на устройстве показала: конвейер резолвится
полностью (16/16/16), в loopItems — 2 обещания DelayIgnoreTimeScalePromise,
но **ни одного с битами 0.35f** — наблюдаемые обещания принадлежат чужим
потокам из реестра выше. Поскольку поток переноса пользователя может идти
через обычный time-scaled `DelayPromise` (его раскладка сдвинута:
initialFrame 0x18 / delayTimeSpan 0x1C / elapsed 0x20), контроллер теперь
распознаёт **оба** класса и пишет 0.0 в поле темпа каждого (тот же
побитовый гейт 0x3EB33333, свои офсеты на класс). Для точной диагностики
фактической константы добавлена накопительная (с момента включения
тумблера) гистограмма битов темпа всех опознанных обещаний — отдельно для
unscaled (DelayIgnoreTimeScalePromise) и scaled (DelayPromise): строка
«Накоп: …» в меню и `acc ign=[…] dpr=[…]` в логе. Гистограмма отвечает на
вопрос «какие темпы реально живут в loopItems во время переноса», даже
когда побитовый фильтр ничего не нашёл.

**Потолок скорости (дизасм `AddAction` 0xc362e68 / `RunCore` 0xc363298 / `MoveNext`
0xc32f8f4, билд `72e26b9`).** Новый промис, созданный вcontinuation во время
прохода раннера, не может быть обработан в том же проходе: при `running=true`
`AddAction` кладёт его в `waitQueue` (+0x40), а прямой путь добавления тоже
берёт `arrayLock` (+0x20), который `RunCore` держит весь проход. Кроме того,
`MoveNext` не завершает ожидание в кадре создания (`elapsed==0 &&
Time.frameCount == initialFrame → ждать`), а `Create` всегда перезаписывает
span=0.35/elapsed=0/initialFrame. Поэтому **гарантированный предел heap-only
подхода — один предмет за проход PlayerLoop (кадр)**; обнуление span достигает
этого предела (первое перемещение — следующий кадр после создания ожидания).
«Весь ящик разом в один кадр» без патча кода невозможен в принципе — только
через частоту кадров (лимит FPS 144 даёт до ~144 предметов/с).

**Как найти заново после апдейта.** Слот — `typeinfo_rva.py` (отпечаток:
4 обращения, статик-полей нет; см. PROVENANCE.md §B). Поля — по читаемым
именам в `il2cpp.h` (`runners`, `loopItems`, `delayFrameTimeSpan`, `elapsed`,
`initialFrame`). Семантика — дизасмом: `DelayIgnoreTimeScalePromise.Create`
пишет span→+0x18, elapsed→+0x1C, initialFrame→+0x20; `MoveNext` завершает при
`elapsed ≥ delayFrameTimeSpan`; константа 0.35f ищется байт-сканом `33 33 B3 3E`
в `.rodata` (якоря VA — PROVENANCE.md §E). Классы сверяются в рантайме
посимвольным чтением `Il2CppClass.name` (+0x10) / `namespaze` (+0x18) —
`io.named()` из weapon-паттерна для слота TypeInfo не годится (нужен голый
`Il2CppClass*`, а не объект).


### 3.14. Аим: маятник первых секунд — окно «в полёте» и единицы задержки

Жалоба 23.09: «мемори аим ходит как маятником первые 2-3 секунды и тач аим
тоже». Разбор (численный стенд `tools/aim/pend_sim.cpp`, петля в КАДРАХ ИГРЫ
при такте оверлея 81 мс — 12 fps, как на устройстве) нашёл три дефекта,
два из которых — перепутанные единицы измерения задержки.

**1. Тач: окно «в полёте» в 8 ТАКТОВ при задержке в КАДРАХ ИГРЫ.** Касание
идёт через систему ввода Android до 100 мс — это 6-8 **кадров игры**
(замер контроллера фарма). Такт оверлея на устройстве — 81 мс, то есть
задержка укладывается в один-полтора **такта**. Окно в 8 тактов вычитало из
остатка 0.65 секунды приказов, которые камера давно отработала: контроллер
недозаказывал, доворот «зависал», потом разворачивался, окно доедало
призраки — ступени и реверсы на захвате (стенд: захват 30° не сходился за
5 с), а на цели, идущей 30°/с, такое окно тащило шлейф 25° («не успевает
за игроком»). Лечение: вычитать из остатка **только последний заказанный
шаг**, пока он не состарился на `kTouchFlightLife = 0.12 с` (время, а не
такты — частота оверлея плавает 12..60 fps). Стенд: захват 0.92 с, реверсов
0, шлейф 5.6° — как без окна вовсе, но со защитой от сдвоенного заказа.

**2. Память: окно «в полёте» не работало вовсе + та же единичная ошибка.**
После записи шага окно сдвигалось и слот `[1]` обнулялся — шаг **никогда**
не попадал в окно, вычет был мёртв (двойной заказ одного доворота). На
границе фаз — игра в бою просядает до ~12 fps, до частоты оверлея, и запись
становится видимой в боксах только через такт — это давало сдвоенный заказ:
камера проскакивала цель, ошибка меняла знак, качели шли, пока игра не
разгонится («первые 2-3 секунды»). Лечение то же: последний заказ живёт
`kMemFlightLife = 0.10 с`, старшие приказы не вычитаются (иначе на 60 fps
оверлея окно съедает скорость ведения — стенд РЕЙДЖ показал шлейф 2.7°).

**3. Память: «камера ответила» мерилось только по оси камеры.** На
устройстве, где ось не читается (cam_st = 0), `responded` не защёлкивался
вовек и потолок шага оставался 2°/такт (24°/с): «память» ползла к цели
1.5 с, всё это время воюя с пальцем игрока. Накопитель сам подсказывает
отклик: мы пишем **абсолютные** углы, и если игра запись приняла (свой ввод
игрок не добавлял), в накопителе лежит ровно записанное (`kMemAckEps =
0.1°`). ACK по накопителю работает при любом cam_st. Потолок после отклика
не включается разом: растёт 0.6 с (`kMemCapRampSeconds`) и ополовинивается
на каждом перескоке через цель — один шаг в 12° при отстающей камере сам по
себе маятник.

**4. Общее для режимов: источник углов переключался порогом без гистерезиса.**
`aim_angles_for` (game.cpp) считает угол и по оси выстрела, и по пикселю, и
берёт ось, пока расхождение ≤ 2.5°. Пока камера ведётся к цели, ось и
матрица вида расходятся на 2-4° (у оси нет визуального отклонения камеры) и
расхождение плавает вокруг порога: источник меняется каждый такт, ошибка
прыгает на величину расхождения — прицел ходит туда-сюда ровно столько,
сколько длится разворот. Теперь переключение через широкую полосу: на экран
уходим при расхождении > 4°, обратно к оси — < 1.5°, между — держимся
прежнего источника.

Стенд `tools/aim/run.sh` прогоняет новый сценарий «ГРАНИЦА ФАЗ» (камера
видит запись через такт оверлея): сходится без раскачки. Численная модель
обоих контроллеров со всеми режимами задержек — `tools/aim/pend_sim.cpp`
(таблицы: захват/перелёт/реверсы/шлейф для окна 8 тактов, времени жизни и
«последнего приказа»).

### 3.15. Аим: жёсткий лок памяти и учёт «в полёте» по поглощению

Жалоба 23.09, второй заход: «тач аим и мемори аим в первые 2 секунды при
наводке двигаются как маятник вправо-влево от цели; сделай чтобы мемори
жёстко лочился на кости — он работает через память и должен быть самым
точным». Окно «последнего приказа» из §3.14 лечило устройство на 12 fps, но
частота оверлея плавает до 60 fps, и там в полёте висело уже 3-4 приказа,
а вычитался один. Диагноз подтверждён новым стендом
`tools/aim/lock_sim.cpp` (сетка: оверлей 1-5 кадров игры на такт × задержка
касания 2-8 кадров × гейн 0.05-0.20): у «последнего приказа» перелёт на
захвате до 34°, до 15 реверсов шага, на быстром оверлее захват не
сходился вовсе.

**1. Память: жёсткий лок абсолютным углом вместо дробного контроллера.**
Накопитель MouseLook принимает абсолютные градусы, и игра сама
разворачивает их в поворот камеры, поэтому вместо «доля остатка за такт +
потолок 2°→12° с рампой 0.6 с + гаситель перескока» (это и был маятник
первых двух секунд — медленное схождение с раскачкой) каждый такт пишется
**абсолютное направление кости**: камера, от которой измерен остаток, плюс
сам остаток (`esp_aim_projection_basis` в game.cpp — база обязана совпадать
с источником углов цели, иначе лок встанет на расхождение источников до 4°;
флаг пиксельного гистерезиса `aim_angles_for` вынесен в файловой
`g_aim_pix_basis`). Запись идемпотентна: пока предыдущая не развернулась,
снова уходит ТОТ ЖЕ абсолютный угол — граница фаз больше не удваивает
доворот. Захват — один такт (стенд: 30° -> 0.1° за 0.25 с при любом
соотношении частот, перелёт 0.00°, реверсов 0; прежний контроллер на той же
сетке давал перелёт до 82° и захват до 2.7 с). Дрожь кости гасится мёртвой
зоной 0.12° и долей 0.4 в полосе ±0.6° у цели: абсолютная запись долей
сходится монотонно, камера ходит меньше самой дрожи (0.06° при ±0.15°,
0.16° при ±0.40°). Стенд `tools/aim/run.sh`: сценарии «ЖЁСТКИЙ ЛОК»
(0.64 с — прицел на кости, без перелёта), «ВВОД ИГРОКА» (лок держит цель,
когда обработчики игры стирают запись каждый кадр), прежние «ГРАНИЦА ФАЗ»,
«ДРОЖЬ», «РЕЙДЖ» (худший остаток на идущей цели 0.35° вместо 1.25°).

**2. Тач: учёт «в полёте» по поглощению + окно минимумом.** Окно «только
последний приказ» заменено двумя оценками, из которых берётся МЕНЬШАЯ по
величине: (а) **поглощение** — заказ копится в градусах и списывается по
фактическому уменьшению остатка (камера отработала приказ ровно тогда,
когда остаток упал; ни предположений о задержке, ни поедания отработанных
приказов); (б) окно приказов моложе `kTouchFlightLife = 0.13 с` —
ограничивает оценку сверху на ведении цели, где остаток не падает и
поглощение само не списывается. Плюс защита знака: оценки покрыли остаток —
новый приказ не выдаётся. На той же сетке: перелёт 0.00° и 0 реверсов во
всех 12 режимах, захват не медленнее прежнего. Заодно исправлен знак
вычета «в полёте» по тангажу (был инвертирован: `errPitch + flightPitch*gp`
при сдвиге `s_lastOrderPitch = -dy` вычитал в противоположную сторону —
ось тангажа перетормаживалась). Подтяжка гейна ускорена 0.15 -> 0.25/такт,
мягкий старт 0.35 -> 0.20 с (основную раскачку теперь гасит учёт полёта).

### 3.16. Аим: маятник при посадке ФПС и резкий лок кости без отставания

Жалоба 23.09, третий заход: «с тач аимом маятник появляется при посадке
ФПС; мемори аим отстаёт от головы и других костей — нужен именно жёсткий
резкий лок на кости».

**1. Диагноз маятника при просадке ФПС.** При 12-15 фпс игры касание идёт
до камеры 2-8 кадров ИГРЫ — до 0.5-0.7 с. Гибрид из §3.15 в этих условиях
ломался двумя способами: (а) таймаут «заказ потерялся» в 0.4 с списывал
заказ, который ещё ЛЕТИТ — контроллер заказывал заново, и по прилёте обоих
камера перелетала цель; (б) окно приказов в 0.13 с пустело раньше прилёта,
и «меньшая из оценок» становилась нулём поверх летящих приказов — тот же
повторный заказ. Корень: обе оценки смотрели на ОСТАТОК, а он при длинной
трубе врёт (остаток может стоять и у применённого приказа — цель сама ушла
за время полёта).

**2. Тач: поток камеры как источник правды.** В общий выбор цели добавлена
оценка поворота камеры между тактами (`AimUpdateFlow`/`aim_camera_flow` в
aim.cpp): медианный сдвиг центров боксов с совпавшим игроком (нужно ≥3
совпадения — движение одного-двух игроков давится медианой). Мир стоит,
поэтому сдвиг всех боксов разом — чистый поворот камеры: рысканье —
минус медианный сдвиг по X на градусы по горизонтали на пиксель, тангаж —
медианный сдвиг по Y на вертикальные градусы на пиксель, клэмп 30°/такт.
Поглощение списывается в первую очередь по потоку (камера повернулась на Х
— ровно Х заказанного отработано, куда бы ни ушла цель), а при <3 боксов
(дуэль 1 на 1) — запасным падением остатка. Остаток перескочил ноль — весь
висящий заказ обнуляется сразу (камера дальше цели, летящий приказ толкнёт
ещё дальше). Что вычитается из остатка, выбирает его величина: на захвате
(|остаток| > 2.5°) правит поглощение при любой задержке трубы, на ведении
у цели — минимум с окном (след короткий и без потока). Таймаут «потерялся»
поднят 0.4 -> 1.0 с (перекрывает худшую задержку трубы с запасом). Полёт
обнуляется при отпускании пальца по потере цели. Стенд `lock_sim.cpp` на
сетке оверлей 1-5 кадров × задержка 2-8 кадров: перелёт 0.00° и 0 реверсов
во всех режимах; дуэль 1 на 1 без потока — захват без перелёта; ведение
30°/с — шлейф ограничен окном; крайний случай «гейн завышен вдвое и оси
камеры нет» ползёт к цели монотонно без маятника.

**3. Память: полный шаг без долей, порог устойчивого дрейфа.** Отставание
давали два хвоста старого лока: вычет мёртвой зоны сажал камеру в 0.12° ОТ
кости, а доля 0.4 в полосе ±0.6° у цели ползла к кости со скоростью ~2°/с —
идущая кость уходила на 5-30°/с, и лок стоял на краю полосы. Теперь шаг —
ВСЕГДА полный остаток: лок бьёт ровно в кость (захват одной записью, на
стенде 30° -> 0.000° остатка). Дрожь кости отсекает пара фильтров: мёртвая
зона 0.12° и полоса «дрожь кости» 0.45° с порогом УСТОЙЧИВОГО
однонаправленного дрейфа в 3 такта (`mem_axis_off(err, streak)`): дрожь
знак чередует и порог не набирает (камера стоит, на стенде шаг 0.000° при
±0.15° и ±0.40°), настоящее движение знак держит и проходит целиком
(медленный дрейф 3°/с — худший остаток 0.24°; рейдж 30°/с — 0.00°;
следование за идущей целью ограничено только задержкой видимости). Ось, по
которой шага нет, пишется из накопителя как есть: база углов на границе фаз
может отставать от него на кадр, и запись «база + 0» дёрнула бы камеру
назад. Стенд `tools/aim/run.sh` зелёный (сценарии «ЖЁСТКИЙ ЛОК», «ДРОЖЬ»,
«МЕДЛЕННЫЙ ДРЕЙФ» обновлены; добавлена проверка «встаёт РОВНО на кость, а
не на краю мёртвой зоны»).

### 3.17. Аим: четвёртый заход по маятнику тача, упреждение 0.010, память без упреждения

Жалоба 24.09: «тач аим всё равно маятник первые 1-2 секунды при наводке».
Плюс просьбы: поднять упреждение тача на 0.010 и убрать упреждение из
мемори-режима.

**1. Диагноз по стенду.** Повторить маятник на стенде удалось тремя
динамическими сценариями захвата, которых не было в сетках: (а) дрожь
кости ±0.4° с самого начала захвата на быстром оверлее — контроллер гонял
палец за дрожью с частотой кадров (110 реверсов шага, видимый маятник у
цели); (б) цель идёт навстречу — заказ, посчитанный по остатку прошлых
тактов, прилетал уже после того, как цель сократила дистанцию (перелёт
2.3°); (в) дуэль 1 на 1 без потока камеры — на ведении убегающей цели
поглощению нечем сливаться, оценка росла без границы и камера отставала
всё дальше.

**2. Три правила захвата (все проверены стендом):**
   * поглощение клэмпится к ТЕКУЩЕМУ остатку — но только когда виден поток
     камеры (без потока падение остатка — единственный сигнал отработки, и
     клэмп списывал бы его дважды: на стенде шлейф ведения 1 на 1 рос с
     5.6° до 63°, с клэмпом только при потоке — 8°);
   * полоса дрожи у цели 0.45° с порогом УСТОЙЧИВОГО однонаправленного
     остатка в 3 такта — та же пара, что в мемори-локе: дрожь знак
     чередует и не проходит вовсе (реверсы 110 -> 0), медленный дрейф
     проходит целиком;
   * доля шага спадает вместе с остатком (от 2.5° к цели, пол 0.40 — ниже
     шаг станет меньше пикселя и прицел залипнет): поздние прилёты длинной
     трубы не перелетают кость (перелёт на дрожи 0.9° -> 0.4-0.5°).
   Арбитраж оценок «в полёте» теперь тоже зависит от потока: с потоком на
   захвате правит поглощение (слив по потоку), без потока оценка ВСЕГДА
   ограничивается окном приказов (первый гибрид) — иначе ведение без
   потока расходится. Итог сетки: перелёт 0.00° и 0 реверсов во всех
   режимах включая динамические; захват 0.3-1.6 с в зависимости от частот.

**3. Упреждение.** `kAimVelocityLead` поднят 0 -> 0.010 кадра вперёд —
компенсирует только трубу касания, не уезжает вперёд по ходу цели. Само
упреждение теперь считается ТОЛЬКО в тач-режиме: мемори-лок каждый такт
пишет в память абсолютный угол самой кости — упреждать нечего, а сдвиг
точки вперёд выглядел бы как «прицел убегает от головы» (жалоба 19.09).
В выборе цели блок упреждения обёрнут в `g_state.aim_mode != AIM_MODE_MEMORY`.

Стенды: `tools/aim/lock_sim.cpp` — новая секция «динамический захват»
(дрожь с начала, навстречу, убегание, чистый 12 фпс); `tools/aim/run.sh`
зелёный, хост-тесты и полный синтаксический прогон зелёные.

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
(`kPackageRelease`/`kPackageBeta` в `jni/src/process.cpp`). После смены версии
клиент переподключается сам (`esp_reset` + поток привязки).

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
   `benzware-sh-arm64-v8a` → забираешь артефакт из CI.
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
| Поля игрока (ники, кости) | `dump_player_diagnostics()` + `diag_done` в `PlayerTextCache` | `benzware_esp_debug.log` | `git show 08b5b6f` (удалена в `e1eb40c`) |
| Цепочка оружия удалённых игроков | `dump_weapon_probe()` + `weapon_probed` в `PlayerTextCache` | `benzware_weapon_debug.log` | `git show eee29a6` |
| Обход реестра Mirror (руды/животные) | `dump_marker_probe()` + счётчики `probe_*` в `rebuild_marker_entities()` | `benzware_marker_debug.log` | `git show 0aa7a3c` |
| Плашка `ESP attach=… pid=… boxes=…` слева сверху | `DrawAttachStatus()` + вызов перед `DrawEspOverlay()` в главном цикле | на экране | `git show 0f1f869` |
| Работа аимбота покадрово (чувствительность, ответ камеры на палец, скорость цели, остаток ошибки) | `AimDebugLog()` + `AimDebug`/`g_aimDbg` в `main.cpp`, выключатель «Отладка аима» | `benzware_aim_debug.log` | `git show ac1270f` |
| **Автофарм покадрово (не удалён — живёт в сборке)** | `namespace farmlog` в `main.cpp`, тумблер «Отладка → Лог фарма» | `Загрузки/farm_debug.log` | — (см. §6.1) |

Все остальные — одноразовые (пишут при первом скане/первых 8 игроках), вызов
ставится рядом с местом, где значение уже посчитано. Лог автофарма — постоянный,
про него ниже.

### 6.1. Лог автофарма (`farm_debug.log`)

> **Примечание (17.09.2026).** Канала `farmlog` в текущем дереве исходников
> НЕТ: в `jni/src/farm.cpp` (контроллер автофарма) ни namespace'а, ни записи
> файла, ни тумблера «Лог фарма» — и в `main.cpp` на момент HEAD его не было.
> Раздел ниже описывает вариант контроллера с логированием, под который
> написан стенд `tools/farm/` (`ctrl_test.cpp` до сих пор ссылается на
> `farmlog::Row`/`g_row` и не собирается — проверки лога в `run.sh` устарели,
> экстракция же режет реальный код из `jni/src/farm.cpp`). Формат строк и
> порядок чтения ниже сохранены как справочник по варианту с логом.

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
  `benzware_aim_debug.log`, чтобы читать глазами и резать `awk`).
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

### Апдейт от 18 сентября 2026 (новый `dump.7z`, коммиты `dc5567f`/`8946e71`)

Дампы: `dump.7z` sha256 `c0b883f3…ae0c` (24 949 124 Б) — новый; `libil2cpp.7z`
sha256 `57f9f051…06a1` (23 546 006 Б) — **тот же файл, что и 13 сентября**,
то есть новый `libil2cpp.so` не приехал (коммит `8946e71` пустой). Прошлый
`dump.7z` — 24 889 304 Б (коммит `5ba910b`). Поколение backing-полей в новом
билде — `K*` (в прошлом было `L*`). Таблицы метадаты: 31 251 типов / 277 497
методов / 137 579 полей (было 31 138 / 275 631 / 136 787).

**Что изменилось (4 константы из 141):**

| Константа | было | стало | как найдено |
|---|---|---|---|
| `EVENT_HANDLER_AIM_ACTIVITY` | 0x270 | **0x290** | по имени `Aim` (via `playerEventHandler`) |
| `KCC_CHARACTER_ANIMATION` | 0x108 | **0xF0** | позиционно (`…_CharacterAnimation_o*`) |
| `KCC_MOVE` | 0x16C | **0x154** | позиционно (`…_Move_o`) |
| `PLAYER_VOICE_STATE` | 0x2E8 | **0x2F8** | позиционно (`Dissonance_VoicePlayerState_o*`) |

**Что НЕ изменилось:** остальные 137 полей и весь блок Unity/IL2CPP ABI —
движок тот же (Unity 6000.3.18f1, metadata v39), значит §3.1 и §3.9 вместе со
всеми `CAMERA_*` / `IL2CPP_*` / нативными `TRANSFORM_*` остаются действительны.

**Переименования классов (значения те же, имена в карте обновлены):**
`Gum`→`brc` (7 констант), `GKo`→`brf` (2), `oh`→`Sq` (2), `Gmv`→`bxt`,
`Gub`→`brB`; по типам: `GqT`→`bLt` (инвентарь), `bp`→`xY` (вид оружия).

**`GKo`→`brf` найден по форме, а не по имени.** Старой структуры в дампе нет,
`update_offsets.py` это показал отдельным предупреждением. Форма та же:
`GameObject` на 0x18 и `RaycastHit` на 0x48 —
`python3 tools/offsets/il2cpp_layout.py --new il2cpp.h find UnityEngine_RaycastHit_o UnityEngine_GameObject_o`.
`GKO_HIT_OBJECT` и `GKO_RAYCAST_HIT` остались 0x18 и 0x48.

**Косвенная проверка класса A:** `beta_offsets.py` (дамп беты против НОВОГО
релизного) по полям расхождений не показал вовсе — бета того же поколения, что
и новый релиз. Разошлись только RVA, что и ожидалось (это другой бинарник).

**Класс B — слоты TypeInfo (вторая половина апдейта, коммит `e478e52`).**
`libil2cpp.7z` пришёл со второго раза: первая перезагрузка дала файл байт-в-байт
равный тому, что лежал с 13 сентября (sha256 `57f9f051…06a1` — та же цифра, что
в записи от 13 сентября). Проверялось содержимым, не только хешем: `.so` из
репозитория со СТАРЫМ `script.json` воспроизводил старое значение 0xD8DB8B8 с
его отпечатком, а с новым — чужой слот. Признак правильной пары: отпечаток
класса повторяется из билда в билд, хотя число обращений меняется.

| Константа | было | стало | отпечаток в новом билде |
|---|---|---|---|
| `PLAYER_MANAGER_TYPEINFO_RVA` | 0xD8DB8B8 | **0xD9841E0** | 11 обращений, статик-поля `[0x1A0x7]` (метка — чтение по 0x1A) |
| `GAME_CONTROLLER_TYPEINFO_RVA` | 0xD8D61E8 | **0xD97F598** | 1 обращение, статик-полей нет |
| `NETWORK_CLIENT_TYPEINFO_RVA` | 0xD8DAB08 | **0xD983448** | 6 обращений, статик-полей нет |
| `TOD_SCAN_RVA_BEGIN/END` | 0xD8D0000/0xD970000 | **0xD970000/0xDA10000** | окно сдвинуто на +0xA8928 |

Сдвиг слотов между билдами почти одинаков для всех трёх классов (0xA8928 /
0xA93B0 / 0xA8940 — разброс 2.7 КБ), окно скана сдвинуто на тот же +0xA8928 и
округлено вниз до 0xD970000. Проверено, что внутрь нового окна попадают все три
слота и оба кандидата TOD_Sky (`tx`, найден по полю `TOD_CycleParameters_o*
Cycle` на 0x40): 0xD988550 и 0xD98B6C8.

**Осторожно: `update_offsets.py --verify` (без старого дампа) берёт ВЕРХНИЙ
кандидат и для `GameControllerBase` он чужой** — 0xD98E2D0 (30 обращений,
`[0x8x15, 0x0x15]`) вместо верного 0xD97F598. Значения ставились вручную по
отпечатку, как и 13 сентября; авторежимом с `--apply` без `--old` пользоваться
нельзя.

**Про `.data.rel.ro`:** слоты лежат в `.data` (в новом бинарнике
0xD71B720..0xDD3A3C8), название секции в комментариях историческое — в коде от
этого ничего не зависит.

### Приведение карты к дампу `72e26b9` (23 сентября 2026)

Репозиторий стартовал (коммит `72e26b9`, 22 сентября) с дампом **новее**
журнального `e478e52`: `dump.7z` sha256 `9670bcf4…bb6` (24 941 685 Б),
`dump.cs` 138 459 888 Б / sha1_12 `02d9673b2d38`, 31 252 типов (было
31 251), поколение backing-полей `B*` (было `K*`). Записи в журнал тогда не
было, а заголовок `game_offsets.h` уже содержал новые значения — проверено
независимо: все три `*_TYPEINFO_RVA` и окно TOD воспроизводятся
`typeinfo_rva.py` с теми же отпечатками (PlayerManager 0xD9C5310 — 6
обращений, `[0x1A0x2]`; GameControllerBase 0xD9C0690 — 3 обращения, без
статик-полей, верхний кандидат 0xD9CF2E0 — чужой; NetworkClient 0xD9C4570 —
6 обращений; окно 0xD9B0000..0xDA50000 накрывает все слоты).

**Ротация имён (значения не менялись):** `brc`→`eDg` (PlayerEventHandler),
`Sq`→`gL` (nicklabel), `bxt`→`eIl` (VoicePlayer), `brB`→`eDw` (Activity),
`brf`→`eDK` (результат луча, снова по форме: GameObject 0x18 + RaycastHit
0x48), backing-поля `K*`→`B*`, прочие обфусцированные поля — по списку в
коммите. Все смещения полей совпали со старым поколением один в один.

**Карта приведена к дампу:** 57 переименований по стратегии «смещение+тип»
(ровно как записано в `src.how` каждой записи; via-цепочки — по читаемым
именам стабильных структур). `verify_map.py /tmp/dump`: **175 полей совпало,
расхождений 0**. `_build` в `offsets_map.json` перезаписан (отпечаток
текущих дампов), `PROVENANCE.md` пересобран (`make_provenance.py`, билд
`72e26b9`, предыдущий `e478e52`).

**Добавлено для мгновенного подбора (§3.13):** `PLAYERLOOPHELPER_TYPEINFO_RVA`
0xD9C52E0 (релиз) / 0xD9A3488 (бета) + поля UniTask-обещания (0x30/0x38/0x18/
0x1C/0x20) и `IL2CPP_CLASS_STATIC_FIELDS` 0xB8. Бета-заголовок перегенерирован
`beta_offsets.py --apply` (207 констант, 6 отличий от релиза, отпечатки
совпали).

### Маятник аима первых секунд: окна «в полёте» (23 сентября 2026)

Жалоба: «мемори аим ходит как маятником первые 2-3 секунды и тач аим тоже».
Диагноз по численному стенду `tools/aim/pend_sim.cpp` (петля в кадрах игры,
такт оверлея 81 мс): (1) тач-окно «в полёте» в 8 тактов при физической
задержке касания в 6-8 КАДРОВ игры — перепутаны единицы, окно вычитало
0.65 с отработанных приказов; (2) окно памяти было мертво (слот обнулялся
вместо записи шага), и на границе фаз (игра ~12 fps в бою) каждый доворот
уходил в накопитель дважды; (3) «камера ответила» требовало оси камеры
(cam_st = 0 на устройстве) — потолок шага навсегда 2°, «память» ползла
1.5 с; (4) источник углов в `aim_angles_for` переключался порогом 2.5° без
гистерезиса — на развороте покадрово. Лечение: оба окна — «последний
заказанный шаг», живущий 0.12 с (тач) / 0.10 с (память); ACK по самому
накопителю (пишем абсолютные углы); потолок памяти растёт 0.6 с и
ополовинивается на перескоке; гистерезис 1.5°/4° у источника углов. Стенд
`tools/aim/run.sh` зелёный (+ новый сценарий «граница фаз»), hostcheck и
остальные хост-тесты пройдены. Известное: стенд `tools/farm/run.sh` сломан
ещё до этой правки (сценарии завязаны на удалённый из farm.cpp farmlog) —
ремонт отдельной задачей.

### Маятник при посадке ФПС и резкий лок кости (23 сентября 2026)

Третий заход по маятнику. Тач: при 12-15 фпс игры приказ летит в трубе
касания до 0.5-0.7 с, и оценки «в полёте» по остатку там врут (остаток
стоит и у применённого приказа, если цель сама ушла). Добавлен ПОТОК
КАМЕРЫ — медианный сдвиг центров боксов с совпавшим игроком (≥3 совпадений)
в общем выборе цели: камера повернулась на Х градусов — ровно Х заказанного
отработано. Поглощение списывается по потоку, по вычету минимума с окном
остаток оценивается только на ведении у цели (`|остаток| ≤ 2.5°`), на
захвате правит поглощение; висящий заказ обнуляется при перескоке остатка
через ноль и при отпускании пальца; таймаут «потерялся» 0.4 -> 1.0 с.
Память: шаг лока — теперь ВСЕГДА полный остаток (без вычета мёртвой зоны и
без долей, которые давали видимое отставание от идущей кости); дрожь кости
отсекает порог устойчивого однонаправленного дрейфа в 3 такта внутри полосы
0.45° (дрожь знак чередует и не проходит вовсе, настоящее движение знак
держит и проходит целиком); ось без шага пишется из накопителя как есть
(база на границе фаз отстаёт на кадр). Стенды: `tools/aim/run.sh` (жёсткий
лок 30° -> 0.000° одной записью, дрожь ±0.4° не двигает камеру вовсе,
дрейф 3°/с держится с остатком 0.24°, рейдж 30°/с — 0.00°) и `lock_sim`
(сетка оверлей 1-5к × задержка 2-8к: перелёт 0.00° и 0 реверсов во всех
режимах, включая дуэль 1 на 1 без потока). Хост-тесты
touch/hostcheck/lang/weapons/viewmodel/log зелёные, полный синтаксический
прогон `tools/syntax/check.sh` без ошибок. Известное: стенд
`tools/farm/run.sh` сломан ещё до этой серии правок — ремонт отдельной
задачей.

### Четвёртый заход по маятнику тача, упреждение 0.010 (24 сентября 2026)

«Тач аим всё равно маятник первые 1-2 секунды при наводке» — воспроизведено
стендом в динамических сценариях захвата: дрожь кости ±0.4° на быстром
оверлее гоняла палец с частотой кадров (110 реверсов шага), идущая
навстречу цель перелеталась на 2.3° (заказ по остатку прошлых тактов), в
дуэли 1 на 1 без потока поглощение не сливалось и камера отставала без
границы. Лечение: клэмп поглощения к текущему остатку (только при видимом
потоке камеры), полоса дрожи у цели 0.45° с порогом устойчивого остатка в
3 такта (та же пара, что в мемори-локе — реверсы 110 -> 0), спад доли шага
у цели (пол 0.40 — ниже шаг меньше пикселя), арбитраж оценок «в полёте»
без потока всегда минимумом с окном. Упреждение тача поднято 0 -> 0.010
кадра; из мемори-режима упреждение убрано вовсе (лок пишет абсолютный угол
кости каждый такт — упреждать нечего). Стенды зелёные: перелёт 0.00° и 0
реверсов во всех режимах сетки, включая динамические.

### Упреждение 0.055, руды по видам, аим и ЕСП на союзников (24 сентября 2026)

Три правки по просьбе пользователя.

**Упреждение.** `kAimVelocityLead` поднят 0.010 -> 0.055. Как и с прошлым
значением, оно применяется ТОЛЬКО в тач-режиме (общий выбор цели, ветка
`aim_mode != AIM_MODE_MEMORY`): мемори-лок пишет абсолютный угол кости каждый
такт, упреждать там нечего.

**Руды по видам.** В «Визуалах» карточка «Цвета руды» перестроена в «Руды по
видам»: у камня, железа и серы теперь свой тумблер (показывать ли этот вид)
плюс свой цвет; главный тумблер «Руда» остался общим выключателем. Фильтр
видов стоит в `esp_get_markers()` поверх сбора (скан всегда собирает все
руды, переключение мгновенное). Вид хранится в `MarkerEntity.ore_kind`
(0/1/2, проставляется из тех же `kOreStone`/`kOreMetal`/`kOreSulfur` и из
пути через лут `marker_from_loot`). Настройки пакуются инверсными битами
64/128/256 в `esp_extra.x` — в старых конфигах там 0, т.е. все виды
включены и поведение не меняется. Диапазон проверки флагов расширен 64 -> 512.

**Союзники: аим и ЕСП.** В предыдущей диагностике признак `box.ally` не
срабатывал, потому что у чужих PlayerManager поля группы приходили пустыми.
Теперь локальный игрок выбирается по точному адресу `g_local_player` в списке
PlayerManager, а не по ближайшей к камере позиции; если точное соответствие
пока недоступно, аим-лог это показывает. Добавлены независимые источники
членства: team/clan/clan tag, TeamData pointer/name, roster ID/ник/clan tag и
Mirror netId. «Союзники» скрывает/показывает подтверждённые ally-боксы и прочие
player-визуалы; аим пропускает их независимо от этого переключателя.
Список обновляется раз в 30 кадров, transient read failures не превращаются в
успешный пустой список и кратко сохраняют подтверждённый roster.

Резолвер проходит вложенный путь `PlayerManager.team -> TeamComponent.team ->
TeamData`; `TEAM_DATA_MEMBERS` сдвигается только при единственном ссылочном
кандидате. Сверены дампы: TeamData name/list имеют одинаковые смещения в
release/beta (`0x10/0x20`), имена полей ротированы; `TeamMember.netId` — `0x2C`.
Проверки: `tools/team_membership_test.sh`, `tools/aim/run.sh` (включает
membership cases), `tools/offsets/run.sh` (59 resolver assertions),
`beta_offsets.py --selftest --no-rva`, host `g++ -fsyntax-only` для изменённых
C++ translation units. Android NDK и устройство в среде не обнаружены, поэтому
APK/device validation не выполнялись; диагностическая строка `esp_get_ally_diag()`
показывает этап, если на реальном клиенте источник всё ещё не читается.

### Автообновление оффсетов: чит сам вычитывает смещения из живой игры (24 сентября 2026)

**Задача.** Раньше после апдейта игры чит молчал, пока оффсеты не обновят
руками по свежему дампу. Теперь чит при привязке сам находит нужные классы и
смещения полей в памяти работающей игры и подставляет их поверх вшитых таблиц —
и для релиза, и для беты, без ожидания ручного апдейта.

**Как устроено** (`jni/src/offset_resolver.cpp`, таблица запросов —
`jni/src/offset_resolver_table.inc`). Раскладка самого рантайма il2cpp
(`Il2CppClass`/`FieldInfo`) от билда игры не зависит, поэтому по ней можно
ходить в любом апдейте:

1. Фаза карт: из `/proc/<pid>/maps` берутся все неисполняемые отображения
   `libil2cpp.so` вокруг выбранной базы (в них лежат слоты-указатели на
   классы) и окно строк `global-metadata` (имена классов/полей).
2. Скан порциями по 4 мс на такт (общий потолок 300 с): по 8-байтовым слотам
   ищутся указатели на `Il2CppClass`. Настоящий класс отличается тем, что его
   `image` смотрит в `libil2cpp.so`, а `name`/`namespace` — в окно метаданных.
   Так собирается карта «короткое имя -> `Il2CppClass*`», заменяющая
   `*_TYPEINFO_RVA`.
3. Калибровка: на живом классе проверяется табличное смещение массива полей
   (`Il2CppClass.fields` = 0x80), не сошлось — перебор рядом. Затем для каждой
   записи таблицы смещение поля читается из `FieldInfo` по имени.
4. Найденное пишется в активные слоты `go_active`. Что не нашлось — остаётся
   на вшитом значении: резолвер никогда не делает хуже, чем было.
5. Второй проход — поля, чьи имена РОТИРОВАНЫ обфускатором
   (`_BYK_k__BackingField` -> `_CQx_k__BackingField` в новом билде). По имени
   они не находятся, поэтому берутся по ТИПУ: пятый столбец таблицы
   (`f4`/`b1`/`u4`/`i4`/`str`/`obj`/`c:ИмяКласса`) сверяется с `FieldInfo.type`
   живого класса — `Il2CppType.data.klassIndex` для ссылочных полей (тот же
   индекс лежит в `Il2CppClass.byval_arg@0x20`) и `Il2CppTypeEnum` для
   примитивов. Единственный кандидат берётся сразу; при нескольких выручает
   вшитое смещение (совпадение и типа, и смещения из дампа — верный признак).
6. Результат пишется в кэш (`<каталог конфигов>/.offs`) и берётся из него при
   следующем входе — см. ниже.

**Константы рантайма** (`IL2CPP_CLASS_*`/`IL2CPP_FIELDINFO_*` в заголовках
оффсетов) подтверждены дизассемблом `libil2cpp.so` (Unity 6000.3.18f1, релиз и
бета одинаково): `Il2CppClass.name@0x10, namespace@0x18, fields@0x80,
field_count@0x124`; `FieldInfo` размером 0x20: `name@0x00, type@0x08,
parent@0x10, offset@0x18`.

**Где используется.** Ядро просит класс у резолвера раньше, чем лезть по
вшитому `TYPEINFO_RVA`: `PlayerManager`, `GameControllerBase` (`game.cpp`),
`NetworkClient` (`game_markers.cpp`), `PlayerLoopHelper` (`loot_memory.h`).
Если резолвер класс не нашёл — код падает на вшитое значение, как раньше.
После переключения релиз/бета `go::SelectBuild` перетирает слоты таблицей —
`offset_resolver_reapply()` возвращает поверх найденное живьём. В настройках
(вкладка «Система») показана строка статуса резолвера.

**Сканирование только при первом входе.** Полный резолв на телефоне — минуты
(образ `.so` сотни МБ, куча 250+ МБ, а такт всего 4 мс), поэтому раньше его
начинало ждать заново после каждого перезахода в игру: переподключение
смотрителем кадра, респавн, перезапуск клиента. Теперь результат резолва
пишется в файл `<каталог конфигов>/.offs` (как `.build`/`.lang`), а при
следующем входе берётся оттуда мгновенно — скан не запускается вовсе. В строке
статуса это видно как «готов (кэш первого входа)».

Ключ кэша — отпечаток самого `libil2cpp.so`: заголовок ELF, программные
заголовки, размер образа из карты процесса и содержимое 16 выборок по 8 КБ
(~128 КБ чтений, доли секунды). Апдейт игры меняет образ — ключ меняется, кэш
не срабатывает, резолв идёт заново. Обратной ситуации (старый кэш на новом
образе) не бывает: без совпадения ключа файл не читается и ни один слот не
трогается.

В кэше и смещения полей, и адреса классов, которые ядро берёт по имени
(замена `*_TYPEINFO_RVA`). Каждый такой адрес при загрузке проверяется: по
нему должен лежать живой `Il2CppClass` с тем же именем. Не сошлось — запись
отбрасывается, смещения всё равно берутся (они от адресов классов не зависят).

Если кэш нужен пересобрать (подозрение, что в нём половина результата):
удалить `.offs` из каталога конфигов — следующий вход отсканирует заново.

**Что НЕ резолвится по именам.** Классы с ротацией имён (обфускатор:
`PlayerEventHandler`, виджет ника, `VoicePlayer`, `Activity`, `eDt`, командные
объекты `euD`/`Reo`) меняют имя от сборки к сборке — их имена в таблице не
задаются, они остаются на ручных значениях. Поля длиной ≤ 3 символов (типа
`id`) генератор тоже пропускает — слишком много тёзок.

Имя класса для поиска генератор берёт из карты: обычно последний токен
(`Oxide_FPManager` -> `FPManager`), но классы, у которых подчёркивание — ЧАСТЬ
имени, а не разделитель пространства имён, сохраняются целиком
(`MineableObjectExtension_OreHitstreaks`, `TOD_CycleParameters`) — список
`KEEP_FULL` в `gen_resolver_table.py`. Ровно на этой обрезке резолвер и терял
классы: лог 24.09 искал `HitMarkerItem`/`OreHitstreaks`/`CycleParameters`,
которых в рантайме нет под такими именами, и 12 полей крестика фарма и времени
суток оставались на вшитых значениях после апдейта игры.

**Стенд** — `sh tools/offsets/resolver_test.sh`: синтетический процесс-«игра»
с настоящим `libil2cpp.so`- и `global-metadata`-отображениями, классами и
массивами полей; настоящий резолвер гоняется по нему через `/proc/<pid>/mem`
и обязан переписать подложенные смещения (27 проверок, прогон 10 раз подряд
без провалов). Помимо резолва по именам стенд проверяет поиск поля по ТИПУ у
поля с «ротированным» именем и весь цикл кэша первого входа (взято из кэша
мгновенно, чужий ключ не применяется). Попутно стенд поймал ДВА настоящих бага:

* **Разбор `/proc/<pid>/maps`.** Строки карт не завершались нулём, и
  `strchr`/`strstr` шли по ВСЕМУ остатку файла: любая строка, после которой
  где-то дальше стоит `global-metadata`, сама считалась метаданными — окно
  метаданных раздувалось на полкарты, а сегменты `libil2cpp.so` терялись
  (плавающий провал: резолвер молчал в ~1 запуска из 3). Исправлено: каждая
  строка копируется в буфер с `\0` на конце.
* **Голодание скана.** При исчерпании бюджета чтений кусок отбрасывался
  недообследованным, и плотные «похожие на указатели» данные съедали классы,
  стоящие дальше в том же куске. Исправлено: скан сдвигает позицию только на
  просмотренное и следующий такт продолжает ровно с места остановки.

Без стенда оба бага ушли бы на устройство в виде «автообновление иногда
молчит» — ровно той неопределённости, ради устранения которой всё и делалось.

### Апдейт игры 26 сентября: автообновление искало метадату не тем способом

**Симптом** (`benzware_offsets.log`, 26.09, pid 14756). После апдейта игры
резолв закончился ничем:

```
привязка: база 0x7744673000 — по ELF-заголовку (вшитые RVA НЕ сработали)
автооффсеты: база 0x7744673000 — сегментов 3 (94 МБ), метаданные НЕ найдены 0x0..0x0
автооффсеты: якорь 0 (слот TypeInfo ещё не инициализирован или RVA сменился)
автооффсеты: классов найдено 768 (по форме 236347, имён 116567, отказов 17201, табличных 0)
автооффсеты: строка «PlayerManager» в памяти НЕ найдена (просмотрено 5301 МБ)
автооффсеты: разрешено 0/160 констант, метаданные НЕ найдены
```

Тем же утром (01:42, лог `benzware.log`) на предыдущей версии игры всё
сходилось: `метаданные найдены 0x773593e000..0x77374cf000`, 146 табличных
классов, `разрешено 208/160 констант`. Разница ровно одна — апдейт игры.

**Почему сломалось.** Окно строк метаданных резолвер умел получать только
двумя способами:

1. строкой в `/proc/<pid>/maps` с именем `global-metadata` — так и работало
   до апдейта;
2. по вшитому `TYPEINFO_RVA` (якорю) — после апдейта RVA уехал.

В новой сборке в карте процесса имени `global-metadata` больше нет: на Android
файл лежит внутри `.apk` и мапится оттуда, а строки карты с `.apk` разбор
маршрутов **вычёркивал целиком** (вместе с `.dex`/`.oat`/`.art`). Плюс на
файловые отображения стоял потолок 1 ГБ. То есть блок метаданных физически не
попадал ни в один список поиска.

Без окна имён фильтр «имя класса лежит в метаданных» выключается, скан ловит
десятки тысяч мусорных структур (те самые 768 «классов» при 236 347
«похожих по форме»), а запасной поиск строки `PlayerManager` в 5 ГБ памяти
отбрасывал даже настоящие попадания: эвристика требовала, чтобы строки шли
ВПРИТЫК (как в таблице строк), а имя в дампе с устройства
(`dumper.7z`, 24.09) окружено нулями и указателями.

Вывод лога «имена зашифрованы» был ошибочным — искали не там и не так.

**Что переделано** (`jni/src/offset_resolver.cpp`).

1. **Блок метаданных ищется по СИГНАТУРЕ.** У файла `global-metadata.dat`
   первые четыре байта — `0xFAB11BAF` (`Il2CppGlobalMetadataHeader.sanity`),
   дальше `int32` версии (24 — Unity 2018/2019, 27 — 2020/2021, 29 — 2022,
   31 — Unity 6). Так его ищут и Il2CppDumper, и Il2CppInspector: «если файла
   нет на обычном месте — ищи эти байты». Новая фаза `PH_META` делает то же по
   живой памяти: сначала мгновенный проход по НАЧАЛАМ всех отображений
   (файл метаданных мапится целиком — заголовок в первых байтах), затем, если
   не нашлось, проход по границам страниц всех диапазонов (заголовок всегда
   выровнен на страницу, поэтому сравнений в 1024 раза меньше, чем у поиска
   строк).
2. **Окно имён берётся из заголовка** — `stringOffset`/`stringSize` (третья
   пара полей, одинаково во всех версиях от 19 до 31). Это ТОЧНАЯ таблица
   строк, а не «весь файл» и не «±32 МБ вокруг имени», поэтому фильтр имён
   снова отсекает мусор. Заголовок проверяется целиком: 30 пар
   (смещение, размер) обязаны укладываться в разумный файл, а по
   `stringOffset` обязана лежать похожая на таблицу строк ASCII-мешанина. Если
   заголовок перетасован протектором, таблица строк ищется перебором пар.
3. **Файловые отображения любых имён** (включая `.apk`, `memfd`, `.dat`
   протектора) собираются в отдельный список — только для сигнатурного поиска;
   в поиск указателей они по-прежнему не идут (там пула классов нет, а
   гигабайтный `.apk` сжёг бы бюджет). Три крупнейших пишутся в лог: по ним
   сразу видно, откуда игра берёт метадату.
4. **Декой-блоки.** Если заголовок настоящий, а табличных имён в его таблице
   строк нет (у протекторов рядом с настоящим файлом бывает подставной),
   поиск продолжается со следующего блока — до трёх попыток.
5. **Эвристика «строки впритык» больше не отбраковывает.** Разреженные
   кандидаты (имя внутри структуры, вокруг нули) принимаются наравне с
   плотными, плотные просто пробуются первыми. Именно эта эвристика превращала
   «нашёл» в «строка не найдена».
6. **Запасной путь «класс рядом с именем».** Дамп с устройства показал третью
   раскладку: имя лежит не в таблице строк, а ИНЛАЙНОМ в структуре (записи по
   40 байт: `char name[31]` + счётчик), а указателя на само имя нет нигде —
   `phase_ptr` такие имена в классы не превращает. Теперь, если проход по
   указателям не подтвердил ни одного класса, проверяются соседи имени: 72
   байта до имени, от ближнего указателя к дальнему, каждый кандидат
   проверяется по форме `Il2CppClass` (образ, массив полей, счётчик, `parent`
   у записей). Не подтвердилось — ничего и не записали.
7. **Порядок фаз.** Если якоря нет, сигнатурный поиск идёт ДО скана `.so`
   (проход по началам отображений — доли секунды), поэтому скан сразу работает
   с точным окном имён. Когда якорь есть (обычный случай), поведение прежнее —
   лишней работы нет.

**Стенд** — `sh tools/offsets/metascan_test.sh`, два сценария по мотивам лога:

* «игра обновилась»: якоря нет, имени `global-metadata` в карте нет, блок
  метаданных лежит страницей ВНУТРИ отображения `base.apk`, рядом подставной
  блок протектора, классы — в анонимной куче. Резолвер обязан найти блок по
  сигнатуре, отвергнуть декой, сузить окно до таблицы строк, найти классы по
  указателям на имена и переписать смещения (14 проверок);
* «метадата зашифрована»: заголовка в памяти нет вовсе, имя есть только
  инлайном в структуре. Резолвер обязан найти имя (старый фильтр его
  выбрасывал) и подтвердить класс по соседству (7 проверок).

Оба стенда, как и `resolver_test.sh`, гоняют НАСТОЯЩИЙ резолвер по
процессу-ребёнку через `/proc/<pid>/mem`. Прогон всего набора:
`sh tools/offsets/run.sh` (в CI строку `sh tools/offsets/run.sh` нужно
добавить в шаг Build руками — правку `.github/workflows` бот в репозиторий
запушить не может).

#### Второй прогон на устройстве (лог 26.09, 17:27–17:33): что сработало и что доделано

Первый же прогон с сигнатурным поиском дал результат: `классов найдено 460
(табличных 451), fields@0x80, разрешено 153/160 констант (19 изменено)`,
союзники применены живьём (`team=0x120 teamName=0x2a0 clanId=0x2b0
clanTag=0x2b8`). Заодно лог ответил на главный вопрос и показал четыре новые
болячки — все четыре вылечены.

1. **Окно метаданных появляется НЕ СРАЗУ.** В 17:27:28 (игра только
   поднялась) — `метаданные НЕ найдены`, а в 17:27:44 то же самое окно уже
   есть в карте: `0x76ab407000..0x76ad000000 (27 МБ)`. То есть в логе 26.09
   утром метадата не была «спрятана в .apk» — её просто ещё не было в карте, а
   вшитый RVA к тому моменту устарел. Теперь перед тяжёлым поиском резолвер
   ПЕРЕЧИТЫВАЕТ `/proc/<pid>/maps` (`recheck_metadata_mapping`, миллисекунды):
   окно, появившееся позже, подхватывается сразу.
2. **Сигнатурный скан жёг полторы минуты впустую.** Постраничный проход по
   8 ГБ шёл даже тогда, когда окно строк уже было известно из карты
   (17:29:18 и 17:31:59 — по 20–90 с). Теперь порядок такой: известное окно →
   сразу разбор таблицы строк; при первом заходе без окна — только мгновенный
   проход по началам отображений, а полный постраничный откладывается до
   конца скана `.so`. Плюс `PH_META` добавлена в `offset_resolver_busy()`:
   смотритель кадра больше не дёргает перепривязку посреди поиска
   («~36 с слепоты — прошу перепривязку» в 17:27:41, 17:28:58, 17:31:40).
3. **Перескан мог сделать ХУЖЕ.** Второй резолв (кнопка «Пересканировать
   оффсеты» в 17:30:37) поймал другой срез памяти: `живых классов 4135`,
   массив классов упёрся в потолок 1024 из одних тёзок и выдал
   `разрешено 5/160`, записав в `PlayerManager.team` чужое смещение `0x98`
   вместо `0x120`. Исправлено с двух сторон:
   * каждый кандидат теперь проверяется ПО ФОРМЕ (`class_shape_ok`): у класса
     с полями обязан быть читаемый массив `FieldInfo` (`parent` смотрит на сам
     класс, тип — валидный указатель, смещение в границах), у класса без полей
     — нулевой указатель массива; плюс потолок в 6 одноимённых кандидатов,
     причём подтверждённый формой вытесняет пустышку и встаёт раньше неё;
   * правило «никогда не хуже»: если новый проход разрешил заметно меньше
     констант, чем предыдущий удачный (меньше трёх четвертей), все слоты
     возвращаются к прежним значениям, а кэш не трогается.
4. **Кэш не сохранялся вообще** — `кэш НЕ перезаписан — якорь 0x0 не живой
   класс` каждые 10 секунд, то есть каждый вход в игру заново платил минутами
   скана. Якорь (класс по вшитому `TYPEINFO_RVA`) нужен только для
   восстановления АДРЕСОВ классов; смещения полей от адресов не зависят.
   Теперь кэш пишется и без живого якоря (если резолв дал хотя бы 8 констант),
   а при следующем входе из такого кэша применяются смещения — чит стартует с
   верными цифрами с первого кадра, — после чего обычный скан доищет адреса
   классов.

Регрессия на стенде: в сценарий «игра обновилась» добавлены 3000 мусорных
структур, у которых на месте имени стоит указатель на настоящее
«PlayerManager», образ валиден, а массива полей нет. Резолвер обязан отсеять
их все и найти настоящий класс (15 проверок вместо 14).

#### Почему после удачного резолва чит всё равно ничего не видел

В том же логе (17:29:40) резолв сошёлся — 451 класс, 153/160 констант, — а
смотритель кадра продолжал писать «240 кадров без кадра» и просить
перепривязку. Причина не в оффсетах, а в ВЫБОРЕ КЛАССА среди тёзок.

Строковый бутстрап находит классы по указателям на имя, и одноимённых
кандидатов получается несколько (generic-инстансы, пустые копии, структуры,
прошедшие проверку по имени). Ядро же брало ПЕРВОГО:

```cpp
const uint64_t found = offset_resolver_class("PlayerManager", "Oxide");
```

Если первым оказывался пустой тёзка, дальше рушилась вся цепочка:
`read_local_player_from_static()` требует `rd_ptr(local_player) ==
g_player_manager_class`, а у объекта класс — настоящий. Локальный игрок не
подтверждался ни одного кадра, значит не было ни камеры, ни позиции: ESP,
аим, фарм, маркеры — всё молчало, хотя смещения полей были уже верные.

Исправлено в трёх точках, все — по одному принципу «проверяй живым чтением, а
не порядком в списке»:

* `resolve_runtime_player_list()` (`game.cpp`) перебирает до восьми тёзок
  `PlayerManager` и берёт того, чья статика отдаёт настоящий список игроков;
  уже выбранный класс перепроверяется каждый раз и сбрасывается, если перестал
  подтверждаться;
* локальный игрок (`game.cpp`): перебираются тёзки `GameControllerBase`, и
  правильным считается тот, чья статика указывает на объект с классом
  `PlayerManager`. Этим же объектом уточняется и сам класс игрока — живая игра
  надёжнее любого списка;
* `resolve_network_client_spawned()` (`game_markers.cpp`) так же выбирает
  `NetworkClient` по живому реестру `spawned` (иначе молча пропадают все
  маркеры мира).

То есть «ничего не работало» — это не одна поломка, а две подряд: сначала
автообновление не находило метадату (и оффсеты, и классы были старые), а
затем, когда метадата нашлась, ядро цеплялось за пустого тёзку.

#### Третий прогон (лог 26.09, 17:56–18:00): игра ещё грузилась, а резолвер сдавался навсегда

Что сработало из предыдущего захода: перечитывание карты нашло окно метаданных,
появившееся уже ПОСЛЕ привязки (`окно метаданных появилось в карте
0x77300ee000..0x7731ce7000 (27 МБ)`), и в нём нашлись все 46 табличных имён.

Что не сработало: по указателям нашлись только системные классы
(`Delegate`, `Dictionary`2`, `MulticastDelegate`), а `PlayerManager`,
`CameraManager`, `NetworkClient`, `KCC` — нет. Итог `разрешено 6/160`. Причина
видна в соседней строке лога:

```
viewmodel: нет локального игрока через GameControllerBase.static:
           cached=0x76c09069a0 rva=0x774c9ebbb8 static=0x0 field=0x10
фрикам: не включён — камера ещё не найдена
```

`static=0x0` — рантайм ещё не инициализировал статику класса. IL2CPP создаёт
`Il2CppClass` и блок статик-полей ЛЕНИВО, при первом обращении: пока игрок в
меню и мир не загружен, классов Assembly-CSharp в памяти просто нет. Резолверу
искать нечего — и это нормально. Ненормально другое: он уходил в `PH_DONE`
навсегда и больше не пробовал, поэтому и после загрузки мира чит оставался
пустым до перезапуска.

Что сделано:

* **Повторный подбор.** Если после прохода нет ключевых классов или у них не
  поднялась статика (`Il2CppClass.static_fields == 0` у `GameControllerBase` /
  `NetworkClient`), резолвер ждёт 25 секунд и пробует снова — до 20 раз.
  Проверка идёт именно по живой статике, а не по факту «класс найден»:
  найденный, но не инициализированный класс ядру бесполезен.
* **Повтор дешёвый.** Окно имён и признак «в .so табличных классов нет»
  переживают повтор, поэтому двухминутный скан `.so` больше не повторяется:
  карта перечитывается заново (появились новые регионы), и сразу идёт разбор
  таблицы строк и поиск указателей.
* **Мусорные «классы» больше не копятся.** Пока окно имён неизвестно,
  структура с читаемой строкой на `+0x10` классом не считается — раньше таких
  набиралось 768 штук, и по ним база объявлялась живой.
* **Строка статуса** в меню теперь честно пишет: `готов частично: полей N/160,
  классов игры нет (мир грузится) — повтор через N с`, а после исчерпания
  попыток — `классы игры так и не появились … нажми «Пересканировать оффсеты»`.

Расписание повторов: первые пять — через 25 секунд, дальше через минуту, всего
двадцать попыток (около 17 минут). Этого хватает и на долгую загрузку мира, и
на вход в мир уже после запуска чита — ровно тот сценарий, в котором лог
17:56 и снимался.

#### Четвёртый прогон (лог 26.09, 18:37–18:48): скан `.so` забивал массив классов

Второй запуск подряд (кэш от первого, удачного): смещения применились сразу
(`кэш дал 129 смещений … классы ищу сканом`), карта перечиталась, окно
метаданных нашлось (`метаданные найдены 0x7735463000..0x773705c000`). А дальше
всё встало:

```
18:41:08  классов найдено 1024 (по форме 297491 …, табличных 256)
18:41:08  не разрешено PlayerManager::teamName (класс отсутствует в кэше)
18:41:09  не найдены классы (45 из 46): CameraManager, …, PlayerManager, …
```

Две ошибки наложились:

1. **Массив классов переполнялся.** Скан `.so` по форме принимает любую
   структуру, у которой указатель имени попадает в 27-мегабайтное окно
   метаданных, — и за две минуты набирает потолок массива (1024). Дальше
   записать некуда: найденный позже настоящий `PlayerManager` просто
   отбрасывался.
2. **Резолвер считал такой скан успешным.** Условие было «нашлись ли
   ВООБЩЕ табличные имена»: среди 1024 мусорных структур нашлись 256 с
   табличными именами (`Delegate`, `Dictionary`2`, `Text`), и строковый
   бутстрап — тот самый, который в удачном прогоне 17:29 дал 451 класс, — не
   запускался вовсе. Повторные попытки (1, 2, 3) повторяли ровно тот же
   двухминутный скан с тем же исходом.

Исправлено:

* **Решения принимаются по КЛЮЧЕВЫМ классам** (`PlayerManager`,
  `GameControllerBase`, `NetworkClient`), а не по числу любых табличных имён:
  нет их — идём в бутстрап, сколько бы `Delegate`-ов ни нашлось.
* **Порядок путей перевёрнут.** Если якоря нет, а окно имён известно, сразу
  идёт поиск по указателям на имена: он сверяет имя точно, покрывает и
  сегменты образа, и кучу, и занимает ~1,5 минуты вместо 2 минут скана по
  форме. Скан `.so` остался запасным путём — запускается, только если
  бутстрап не дал ключевых классов.
* **Табличное имя всегда влезает в массив:** при переполнении оно вытесняет
  нетабличную запись, а тёзок одного имени держим не больше шести.

Заодно лог подтвердил, что защита «никогда не хуже» работает:
`новый резолв ХУЖЕ прежнего (2 против 131 констант) — вернул прежние значения`.

### Ускорение подбора оффсетов (26 сентября 2026)

Замер по логам: проход по указателям читал **5312 МБ за ~65 с ≈ 80 МБ/с**, и
это был почти весь подбор. Узкое место не в железе: резолверу выдавалось 4 мс
из каждых 16,7 мс кадра, то есть он работал четверть времени.

Что сделано (всё — без потери полноты: любой быстрый путь имеет откат на
полный перебор):

1. **Разведка диапазонов.** Перед проходом по указателям делается дешёвая
   выборка: 8 проб по 16 КБ на диапазон (~60 МБ на все 450), считается
   плотность значений, смотрящих в окно строк. Обход идёт по убыванию
   плотности. В логах классы жили в двух-трёх диапазонах из сотен
   (`0x7710000000..0x7717000000 — 346370 попаданий` против единиц у
   остальных), так что нужный регион теперь первый, а не двухсотый.
2. **Ранняя остановка.** Как только найдены ключевые классы
   (`PlayerManager`, `GameControllerBase`, `NetworkClient`), досканируется
   192 МБ соседей — и проход заканчивается. Раньше он шёл до конца всех
   диапазонов или до потолка 6 ГБ.
3. **Память «горячих» диапазонов.** Диапазоны, где классы нашлись, помнятся в
   пределах процесса и при повторном подборе проверяются первыми.
4. **Локальный доскан.** Если часть табличных классов не нашлась, вместо
   нового полного прохода сканируется куча вокруг уже найденного класса:
   рантайм выделяет `Il2CppClass` из общего пула подряд, соседи находятся за
   секунды.
5. **Квант времени по состоянию чита.** Пока ключевых классов нет, чит
   ничего не рисует — квант поднят с 4 до 12 мс (втрое быстрее). Как только
   классы есть, возвращается 4 мс, чтобы не трогать плавность кадра.
6. **Чтения крупнее.** Порция выросла с 256 КБ до 1 МБ (вчетверо меньше
   syscall'ов), пределы «дорогих» чтений на порцию подняты пропорционально,
   а буфер теперь один общий на все фазы вместо шести статических.
7. **Замер в логе.** Каждая фаза печатает своё время и скорость:
   `фаза «поиск указателей» — 12.4 с, 980 МБ (79 МБ/с)`. Дальше оптимизировать
   можно по цифрам, а не на ощупь.

Ожидаемо на устройстве: первый подбор после апдейта — секунды-десятки секунд
вместо 1,5–4 минут; повторный (когда мир догрузился) — считанные секунды.

### Блок «Оружие» без списка build-id (26 сентября 2026)

**Как было.** Без-отдача и без-разброс работали только на сборках из
жёсткого списка `kLayouts` (`jni/src/weapon_tuning_memory.h`): 20-байтовый
ELF build-id + четыре «сторожевые» инструкции по фиксированным RVA. Любой
апдейт игры — и блок молча выключался: `weapon data: отказ=1 (неизвестный
build-id), id=752824b2…0dac`. Чтобы включить, нужно было присылать свежий
`libil2cpp.so` и вручную добавлять запись в таблицу.

**Почему так было.** Цепочка до нужных float'ов и сами смещения записи были
литералами: `weapon+0x130` (конфиг), `config+0x68` (отдача), `config+0x60`
(разброс), запись по `recoil+0x18…0x2c` и `dispersion+0x20/0x24`. Ошибиться
тут дорого: в `HitscanWeaponConfig` на `+0x18` лежит **damage**. Список
build-id и был страховкой от «раскладка уехала — пишем не туда».

**Как стало.** Все эти смещения вынесены в обычные константы и заведены в
карту оффсетов, то есть их теперь держит свежими то же автообновление, что и
остальные 160:

| константа | класс и поле |
|---|---|
| `FPHITSCAN_CONFIG` | `Oxide.FPHitscan.config` |
| `WEAPON_CONFIG_RECOIL` | `HitscanWeaponConfig.recoilConfig` |
| `WEAPON_CONFIG_DISPERSION` | `HitscanWeaponConfig.dispersionConfig` |
| `RECOIL_CONFIG_MIN` / `_MAX` | `RecoilConfig.MinRecoil` / `MaxRecoil` (Vector2) |
| `RECOIL_CONFIG_MIN_SIDE` / `_MAX_SIDE` | `RecoilConfig.MinSideDelta` / `MaxSideDelta` |
| `DISPERSION_CONFIG_MIN` / `_MAX` | `DispersionConfig.minDispersion` / `maxDispersion` |

Имена классов и полей читаемые (`HyperHug.Games.Oxide.Features.Weapons.*`) и
совпадают в релизе и бете — резолвер находит их так же, как `PlayerManager`.
Сверено по обоим дампам: раскладка `RecoilConfig` (0x18/0x20/0x28/0x2C) и
`DispersionConfig` (0x20/0x24) в них одинаковая.

**Допуск на незнакомой сборке.** Если build-id в списке нет, блок больше не
отказывает сразу, но включается лишь при выполнении всех условий:

1. **вся цепочка смещений получена живьём** — `offset_resolver_is_live()` для
   каждой из двенадцати констант (вшитое значение в незнакомом билде не
   считается основанием для записи);
2. классы совпали по имени И пространству имён: `PlayerManager`,
   `NetworkIdentity` (с флагом isLocalPlayer), `FPManager`, `FPHitscan`,
   `HitscanWeaponConfig`, `RecoilConfig`, `DispersionConfig`;
3. сошлись перекрёстные ссылки: `config.recoilConfig == recoil`,
   `config.dispersionConfig == dispersion`, `calc+0x18 == config`,
   `calc+0x10 == dispersion`, а также `weapon+0x200 == recoil`, если читается;
4. значения правдоподобны (`finite`, |v| ≤ 1000), запись идёт только в
   ненулевое поле, после записи — обратное чтение, а при выходе/смене оружия
   оригиналы возвращаются (это было и раньше).

Известные сборки из `kLayouts` по-прежнему идут быстрым путём со сторожевыми
инструкциями — там ничего не изменилось.

Итог: свежий дамп для блока «Оружие» больше не нужен. Он понадобится только
если игра ПЕРЕИМЕНУЕТ эти классы или поля — тогда резолвер честно ничего не
найдёт, `offset_resolver_is_live()` вернёт false, и блок откажет, а не
напишет наугад.

#### Регрессия 26.09 19:46: мусорные классы доехали до слотов (и до кэша)

Лог с устройства: чит долго искал, ESP-боксы прыгали в стороны от игроков,
игра лагала, чит упал. Причина — одна строка в начале лога:

```
19:46:23  автооффсеты: окно метаданных задано по найденному классу «gL»:
                       0x774c000000..0x7750000000 (64 МБ)
```

`gL` — двухбуквенное ротированное имя, которое есть в таблице резолвера
(`PLAYER_NICKLABEL` ищет поле у класса `c:gL`). Старая эвристика ставила окно
имён по ЛЮБОМУ совпавшему табличному имени, и здесь она поймала мусор. Дальше
посыпалось всё: в фальшивое окно «попали» 769 структур, они сошли за классы,
из них посчитались 254 «смещения» (`team=0x2a8`, `clanId=0x78` вместо
`0x120`/`0x2b0`), их применили — и записали в кэш. С такими цифрами ESP читает
кости и позиции не там (боксы прыгают), а чтения по битым указателям валят
кадр и давят игру.

Что исправлено:

* **Окно имён «на глаз» — только по ключевому классу.** Имя должно быть не
  короче 12 символов, входить в короткий список (`PlayerManager`,
  `GameControllerBase`, `NetworkClient`, `PlayerInventory`,
  `CharacterAnimation`) и структура обязана пройти проверку формы.
* **Нет ни одного ключевого класса — нет и результата.** Если после прохода
  не нашлось ни `PlayerManager`, ни `GameControllerBase`, ни `NetworkClient`,
  все применённые в этом проходе значения откатываются, а кэш не пишется.
  Частичный результат (нашёлся один из трёх) по-прежнему принимается — это
  нормальная недогрузка мира.
* **Отравленный кэш обезврежен.** Формат файла поднят до `format 3`; файлы
  второго формата (их мог собрать прежний код) не применяются вовсе — один
  скан, и кэш перезаписывается уже проверенным.
* **Нагрузка на игру снижена.** Квант подбора, пока чит слеп, — 6 мс вместо
  12; добавлен потолок чтения 192 МБ/с (на устройстве сигнатурный проход шёл
  на 727 МБ/с и душил игру); порция уменьшена с 1 МБ до 512 КБ, а дедлайн
  проверяется теперь и ВНУТРИ порции — такт больше не залипает на разборе
  полумегабайта.

Стенд: третий сценарий «мусор» — 4000 фальшивых структур с короткими
табличными именами и полем `team` со смещением `0x2a8` из того самого лога.
Резолвер обязан не применить ничего и не записать кэш (4 проверки).

#### Фризы кадра оказались логом, а не сканом (лог 26.09 20:09)

Профиль кадра показывал чудовищные пики: `peak=960.7ms peakStages[ESP=956.6]`.
Выглядело как «ESP тормозит», но `offset_resolver_tick()` вызывается ИЗ той же
стадии, и виноват оказался не скан, а его собственный лог:

```
20:11:04.026  автооффсеты: не разрешено DispersionConfig::maxDispersion …
…156 строк…
20:11:04.696  автооффсеты: не найдены классы (46 из 50): …
```

156 записей в журнал за 0.67 секунды — и всё это в ОДНОМ кадре. Кадр вставал
на секунду, позиции игроков успевали устареть, и боксы ESP прыгали по экрану;
второй такой же пик (`ESP=448`) давала проба сотен регионов без бита чтения
при разборе карты.

Исправлено:

* неразрешённые поля и неподтверждённые via-цепочки печатаются **пачками**
  (несколько имён в строке) с потолком в 40 записей — вместо 187 отдельных
  строк остаётся 5–6;
* проба «скрытых» регионов в фазе карты сокращена с десяти точек на регион до
  четырёх и ограничена общим потолком 512 проб за проход.

Заодно в `weapon data` добавлена расшифровка второй ступени цепочки: `отказ=11`
теперь сопровождается подшагом (нет `fpManager`, оружие не `FPHitscan` —
топор/кирка, не сходится обратная ссылка и т. д.) и адресами звеньев.

#### Краш при включении блока «Оружие» (26.09, 20:26)

Моя же регрессия из предыдущего шага. В generic-режиме (сборки нет в
`kLayouts`) указатель `layout` остаётся нулевым, а код звал

```cpp
if (!resolve(player,*layout,next)) …      // разыменование nullptr
… apply(0,6,player,*layout) …
```

До сих пор это не стреляло только потому, что цепочка рвалась раньше (шаг 2,
«в руках не огнестрел»). Стоило взять огнестрел и включить любую функцию блока
— мгновенный SIGSEGV. Исправлено: `resolve()`/`apply()` принимают `const
Layout*`, и при `nullptr` имя класса-калькулятора берётся по пространству имён
(`io.calculator_class`), как и задумывалось для generic-режима.

#### Тач-аим: маятник после рваных кадров

Контроллер построен на жёсткой модели задержки — касание доезжает до камеры за
2–4 кадра, и «в полёте» вычитается из ошибки. Когда кадр вместо 17 мс занимал
100–960 мс (см. предыдущий раздел: виноват был лог самого резолвера), эта
модель разваливалась: поза цели и учёт «в полёте» устаревали на вечность, шаг
считался по мёртвым данным и гарантированно перелетал — отсюда «маятник влево-
вправо» и перелёты через цель.

Помимо устранения самих фризов добавлена прямая страховка: на такте с
`dt > 45 мс` шаг не выдаётся вовсе — палец держится на месте, счётчики дрожи
сбрасываются, и следующий нормальный кадр считает ошибку по свежим данным.
Потеря — один кадр наведения, выигрыш — отсутствие перелёта после каждого
подвисания.

### Свой якорь: второй вход без скана (26 сентября 2026)

Жалоба «оффсеты не сохраняются, каждый раз жду скан» была наполовину верной:
смещения полей кэш отдавал мгновенно (`кэш дал 255 смещений`), а вот АДРЕСА
классов — нет. Они живут в куче и меняются при каждом запуске, поэтому в кэше
хранятся относительно якоря — класса, читаемого по фиксированному слоту в
`libil2cpp.so`. Этим слотом был вшитый `TYPEINFO_RVA`, а после апдейта игры он
мёртв — значит, каждый вход заново искал классы сканом.

Теперь резолвер заводит якорь сам:

1. после удачного резолва (и только если вшитый якорь мёртв) запускается фаза
   `PH_SLOT` — порционный поиск в неисполняемых сегментах образа адреса, по
   которому лежит указатель на найденный `PlayerManager`;
2. найденный слот пишется в кэш строкой `anchorslot <RVA> <класс>`;
3. при следующем входе резолвер читает `база + RVA`, сверяет имя класса и
   форму структуры — и это готовый живой якорь: все адреса классов из кэша
   восстанавливаются одним сдвигом, скан не запускается вовсе.

Фактически это автообновление самого `TYPEINFO_RVA`. Если слот ещё не
заполнен (игра не обращалась к классу), работает прежнее ожидание якоря, а не
полный скан.

### Мини-профиль нагрузки: кто виноват в лагах

Раньше в логе была только наша сторона (`кадр-профиль`), и понять, лагает игра
сама или из-за чита, было нечем. Добавлена строка раз в 5 секунд:

```
нагрузка: игра 31 кадр/с, оверлей 59 кадр/с, наш кадр 17.0 мс
          (из них чтения памяти 3.2 мс), чтений 498/кадр, поток 12.4 МБ/с,
          подбор оффсетов не идёт
```

* **игра N кадр/с** — кадры САМОЙ игры, посчитанные по смене матрицы вида
  (не зависит от нашего цикла отрисовки);
* **чтения памяти X мс** — сколько из нашего кадра ушло внутрь syscall'ов
  чтения чужой памяти (замер добавлен в `mem_io.h`);
* **поток МБ/с** — сколько полосы памяти отнимает чит.

Если «игра» проседает при малом «чтения памяти» и «поток» — тормозит сама
игра; если проседает вместе с ростом наших чисел — виноват чит, и видно,
какой именно стадией.

### Тач-аим: такт без нового кадра игры

Оверлей идёт 60 раз в секунду, игра рисует 30–40 — то есть примерно каждый
второй такт контроллер видит ТУ ЖЕ позу камеры. Для него это выглядит как
«поправка не подействовала», и он заказывает ещё столько же; камера приходит
на сумму и перелетает, ошибка меняет знак — маятник влево-вправо.

Теперь, пока есть неотработанный заказ и камера не шевельнулась, шаг не
выдаётся (палец держится на месте, не дольше восьми тактов подряд). Плюс в лог
добавлена диагностика контроллера раз в 0.25 с:

```
тач-аим: тактов 15, шагов 7, реверсов 0, пропущено без кадра игры 6,
         рваных кадров 0, |ошибка| сред 0.42° макс 1.90°, шаг сред 6.1 px,
         гейн yaw 0.0970 pitch 0.0970 град/px (выучен)
```

По ней видно главное: сколько реверсов (маятник), сколько тактов ушло впустую
и не завышен ли выученный коэффициент град/px — типичная причина перелётов.

### Релиз и бета под одним именем пакета: выбор профиля по Build-ID (26 сентября 2026)

Пакет у релиза и беты одинаковый, поэтому по имени процесса версию не
отличить. Единственный надёжный признак — GNU Build-ID образа `libil2cpp.so`
(первые 8 байт заметки, читаются по `base + 0x318`).

Лог 26.09 21:31 показал, во что это выливалось:

```
привязка: база 0x7602290000, Build-ID 0x81d4cb8dcd635ed8 (активный профиль Beta)
привязка: база … верна по токену TypeInfo PlayerManager (RVA 0xdae42b8, профиль Beta)
автооффсеты: найден токен TypeInfo — жду инициализации класса игрой
```

`0x81d4cb8dcd635ed8` — это первые восемь байт build-id
`d85e63cd8dcbd481…`, то есть РЕЛИЗНЫЙ образ из `libil2cpp.7z`. А чит ушёл на
бета-профиль: в таблице Build-ID были только бета-идентификаторы, образ
провалился дальше в перебор RVA, и там первым же сработало слабое совпадение
по токену — бета-RVA стоят в списке раньше релизных.

Исправлено:

* **таблица Build-ID знает обе ветки** — бету (`76fbd4ff…`, `e1079534…`) и
  релиз (`d85e63cd…` из дампа и `1536f42e…` — релиз после апдейта 26.09);
  профиль выбирается по точному совпадению образа, а не по догадке;
* **неизвестный Build-ID больше не меняет профиль** — пишем в лог «игра
  обновилась», оставляем текущий профиль и полагаемся на автообновление;
* **порядок проверок RVA исправлен**: сначала ЖИВОЙ класс у всех кандидатов,
  и только потом токен. Раньше оба условия проверялись в одном цикле, и токен
  первого кандидата побеждал настоящий живой класс четвёртого;
* совпадение по токену выбирает профиль предварительно и не пишет его в файл
  (см. предыдущий раздел).

### Картофельная графика без патчей библиотек (26 сентября 2026)

**Идея.** В URP настройки качества лежат не в коде, а в обычном
ScriptableObject — `UniversalRenderPipelineAsset`, — и пайплайн перечитывает
их КАЖДЫЙ кадр при сборке кадровых данных. Значит понизить графику можно
чистой записью в объектные данные, ровно как «Всегда день» или блок «Оружие»:
ни одной инструкции в библиотеках не трогаем.

**Как находится живой ассет.** Без сканирования памяти:

```
класс UnityEngine.Rendering.RenderPipelineManager  (находит автообновление)
      -> static_fields (+0xB8)
      -> s_CurrentPipelineAsset (+0x8)      = живой UniversalRenderPipelineAsset
```

Ассет проверяется по содержимому: масштаб рендера обязан быть в 0.1…4, а
дальность теней в 0…5000 — чужой объект такую пару не даст.

**Что пишется** (всё — поля того же ассета, смещения из дампа и под
автообновлением по именам полей):

| поле | смещение | значение |
|---|---|---|
| `m_RenderScale` | 0x68 | ползунок 0.40…1.00 (по умолчанию 0.70) |
| `m_ShadowDistance` | 0xC4 | 0 — тени не рисуются |
| `m_ShadowCascadeCount` | 0xC8 | 1 |
| `m_MSAA` | 0x64 | 1 (сглаживание выкл) |
| `m_MainLightShadowsSupported` | 0x9C | 0 |
| `m_AdditionalLightShadowsSupported` | 0xAC | 0 |
| `m_AdditionalLightsRenderingMode` | 0xA4 | 0 (доп. источники выкл) |
| `m_AdditionalLightsPerObjectLimit` | 0xA8 | 0 |
| `m_SupportsHDR` | 0x5D | 0 |
| `m_RequireDepthTexture` | 0x54 | 0 |
| `m_RequireOpaqueTexture` | 0x55 | 0 |
| `m_EnableLODCrossFade` | 0x78 | 0 |
| `m_ReflectionProbeBlending` / `BoxProjection` | 0xC0 / 0xC1 | 0 |

Больше всего даёт `m_RenderScale`: 0.7 — это 49% пикселей, то есть примерно
вдвое меньше работы фрагментному конвейеру; следом идут тени и доп. источники.

**Безопасность.** Оригиналы сохраняются при первом включении, возвращаются при
выключении тумблера и при смене ассета (перезагрузка мира). Пишутся только
значения, которые отличаются от нужных, — лишних записей в память игры нет.
Все смещения заведены в карту оффсетов и таблицу резолвера, поэтому переживут
апдейт игры так же, как остальные 180.

**Где включается.** «Разное» → «Мир» → блок «Графика»: тумблер «Картофельная
графика» и ползунок «Масштаб рендера».

#### Краш FORTIFY в логе резолвера (26 сентября 2026)

```
FORTIFY: vsnprintf: size 18446744073709551614 > SSIZE_MAX
Aborted
```

`18446744073709551614` — это `(size_t)-2`. Классическая ловушка `snprintf`: он
возвращает длину, которая ПОНАДОБИЛАСЬ БЫ, а не записанную. Код вида

```cpp
hn += (size_t)snprintf(buf + hn, sizeof(buf) - hn, "%02x ", raw[i]);
```

при обрезке уводит позицию за конец буфера, и следующий вызов получает
отрицательный размер — рантайм Android валит процесс. В резолвере таких мест
было пять: дампы байт якоря (три штуки), список ненайденных имён и мои же
новые пачки «не разрешено»/«via не подтверждено».

Все переведены на общий безопасный аппендер `log_append(buf, cap, used, fmt,
…)`: он прибавляет к позиции не желаемую длину, а фактически записанную, и
всегда оставляет строку завершённой. В стенд добавлены три проверки — длинные
строки в 16-байтовый буфер и буфер в один байт (69 проверок вместо 66).

#### «Скан не начинается» — кэш ждал мёртвый якорь (лог 26.09, 22:19)

Хорошая новость из этого лога: первый проход отработал ровно как задумано.

```
22:19:17  привязка: база … Build-ID 0x81d4cb8dcd635ed8 (активный профиль Release)
22:19:17  привязка: база … подтверждена по Build-ID релиза
22:19:19  автооффсеты: метаданные найдены 0x776446f000…
22:19:19  автооффсеты: якорь 0x7711b26840 («PlayerLoopHelper») подтверждён как живой класс
22:19:20  автооффсеты: ВСЕ табличные классы найдены в куче (83 табличных)
22:19:20  автооффсеты: фаза «скан кучи» — 1.0 с
```

Профиль выбран по Build-ID верно, скан кучи занял **одну секунду**. Но игра
была в меню, поэтому ключевых классов ещё не существовало: `разрешено 29/184`,
и этот тощий результат ушёл в кэш. Через десять секунд игра домапила настоящий
образ, база сменилась — и дальше полторы минуты шло одно и то же:

```
22:19:32  автооффсеты: процесс/база сменились (…0x773e602000 -> …0x7612290000), резолв заново
22:19:36  автооффсеты: кэш — старый якорь 0x7712229e20, новый 0x0 (НЕ живой): жду инициализации
… каждые 4 секунды до конца лога …
```

Кэш подошёл по ключу образа, но его якорь (адрес класса со СТАРОЙ базы) на
новой базе не читается, и резолвер честно ждал 90 секунд, не начиная скан.
Снаружи это и выглядит как «скан не начинается».

Исправлено:

* **если в этом же процессе резолв уже сходился** (сменилась только база —
  игра домапила настоящий образ), ожидание кэш-якоря пропускается: классы
  никуда не делись, а скан кучи стоит около секунды;
* ожидание кэш-якоря сокращено с 90 до 20 секунд;
* **неполный резолв больше не пишется в кэш**: нет всех ключевых классов —
  нет и записи. Именно кэш из 29 значений, снятый в меню, и подменял собой
  полноценный подбор на следующем входе.

### Разбор четырёх жалоб по логу 26.09 22:28

**Картофельная графика «не работает», а ползунок даёт разноцветные квадраты.**
Строка из лога всё объясняет:

```
картофель: ассет 0x78c8243450, было scale=0.54 тени=50 м MSAA=1 каскадов=1 HDR=0 глубина=0 непрозрачные=0
```

Игра УЖЕ рендерит в 54% разрешения, сглаживание и HDR у неё выключены. А
ползунок по умолчанию стоял на 0.70 — то есть мы делали картинку **чётче и
тяжелее**. Плюс масштаб переписывался на каждом кадре, пока палец ведёт
ползунок: URP на каждое изменение пересоздаёт цели рендера, и десяток
пересозданий в секунду — это те самые цветные квадраты.

Исправлено: масштаб теперь **никогда не поднимается выше исходного** игры
(берём минимум), квантуется шагом 0.05 и пишется не чаще раза в полсекунды.
Выигрыш на этой сборке дают тени (были 50 м → 0) и отключение дополнительных
источников света, а не разрешение.

**Тач-аим «трясёт маятником».** Диагностика показала устойчивую картину:

```
тач-аим: тактов 15, шагов 12, реверсов 1, пропущено без кадра игры 0,
         |ошибка| сред 5.4° … гейн yaw 0.0531 pitch 0.0463 град/px (выучен)
```

Реверс в каждом окне 0.25 с (то есть 4–8 в секунду), шаг выдаётся почти на
каждом такте, а выученный коэффициент град/px скачет между окнами (0.053 →
0.044 → 0.050 по рысканью). Одиночный гаситель (срезать шаг вдвое на реверсе и
восстановиться за четыре такта) такую раскачку не держит.

Добавлен **накопительный трим**: каждый реверс срезает шаг ещё на 15%,
спокойные такты возвращают по 1.5%, предел — 0.35. Устойчивая раскачка теперь
сама себя душит за секунду, а одиночный реверс почти не влияет. Значение трима
печатается в той же строке лога.

**Сайлент «иногда не попадает, особенно вдали».** Активность у него в порядке
(`активен=1 ось=1 записей=496`), значит дело в точке. Чтобы не гадать, добавлен
замер промаха: когда игра отмечает новый выстрел, чит сравнивает точку
попадания (`PlayerManager.LastLocalHitPoint`) с точкой, куда целились:

```
сайлент выстрел: промах 1.83 м (цель на 84.2 м), цель [x y z], попадание [x y z]
```

По этим числам сразу видно, промах это по горизонтали (упреждение), по
вертикали (баллистика) или пуля вообще ушла в другую сторону.

**Быстрый прицел.** Статус в логе `ads=2` — то есть запись проходит и функция
считает себя активной, а визуально ничего не меняется. Поля на месте
(`FPManager.m_AimSpeedMultiplier` 0x34 и `<BYK>k__BackingField` 0xA8 совпадают
с дампом), значит скорость прицеливания в этой сборке задаётся не ими.
Разбор продолжу по следующему логу — нужен кадр с включённой функцией и
свежая строка `viewmodel:` рядом с выстрелом.

#### Лог 26.09 22:48: тач-аим стал точнее, но остался предельный цикл

Накопительный трим и стабилизация коэффициента дали результат: ошибка упала с
5.4° до 0.5–1.0°, коэффициент перестал скакать (было 0.053/0.046 между окнами,
стало ровно 0.0560/0.0560). Но реверсы остались — по одному на каждые 15
тактов:

```
тач-аим: тактов 15, шагов 14, реверсов 1, |ошибка| сред 3.05° макс 5.88°, трим 0.95 …
тач-аим: тактов 15, шагов 12, реверсов 1, |ошибка| сред 0.48° макс 1.03°, трим 0.99 …
```

Это **предельный цикл**: контроллер выдаёт шаг почти на каждом такте, хотя
остаток уже меньше градуса, и каждый такой шаг перекидывает знак ошибки.
Трим при этом восстанавливался быстрее, чем копился (за четыре спокойных такта
возвращался к единице), поэтому и не давил раскачку.

Исправлено:

* **мёртвая зона растёт до амплитуды качки**. Величина ошибки, на которой
  происходит реверс, запоминается (EMA), и мельче 70% от неё аим не двигается
  вовсе. Потолок расширения — 2.5°, так что настоящие поправки не теряются, а
  «охота» внутри собственной тряски прекращается;
* **трим восстанавливается втрое медленнее** (0.6% за такт вместо 1.5%): после
  серии реверсов шаг остаётся подрезанным пару секунд, а не четыре такта.

#### Картофельная графика: ползунок упирался в потолок

В логе снова `было scale=0.54`, а ползунок по умолчанию стоял на 0.70. Правило
«не поднимать выше исходного» превращало включение функции в пустышку: 0.70
обрезалось до 0.54, то есть до того же, что и было. Значение по умолчанию
снижено до **0.5**, и раз в секунду в лог пишется, что реально лежит в ассете:

```
картофель: сейчас в ассете scale=0.50 (просили 0.50, у игры было 0.54)
           тени=0 м главный свет=0 MSAA=1 доп.свет=0
```

Если игра будет возвращать свои значения обратно — это станет видно сразу по
этой строке.

#### Сайлент: замер промахов дал диагноз (лог 26.09, 22:51)

Строка `сайлент выстрел: промах …` набрала 40 выстрелов, и картина
однозначная:

| дистанция | выстрелов | медиана промаха | промахов > 1 м |
|---|---|---|---|
| 0–10 м | 20 | **0.20 м** | 1 (5%) |
| 10–25 м | 16 | **0.34 м** | 2 (12%) |
| 25–50 м | 2 | 101 м | **2 (100%)** |
| 50–200 м | 2 | 91 м | **2 (100%)** |

Вблизи сайлент кладёт пули в кость (20–34 см — расстояние от центра кости до
точки касания коллайдера, это норма). А «промах 91–119 м» означает, что
`LastLocalHitPoint` не обновился вовсе: игра пишет туда точку только по факту
попадания, то есть **дальше 25 метров выстрелы не попадают никуда**.

Причин может быть три: пуля не долетает (`HitscanWeaponConfig.MaxDistance`),
уходит в конус разброса (`DispersionConfig`) или цель уходит за время полёта.
Чтобы их различить, лог теперь отделяет «промах» от «попадания нет» и печатает
баллистику текущего оружия:

```
сайлент выстрел: ПОПАДАНИЯ НЕТ, цель на 64.3 м; оружие: дальность 40 м,
                 базовый разброс 0.030, конус 0.010..0.120
сайлент выстрел: промах 0.33 м (цель на 5.3 м), оружие бьёт на 40 м,
                 конус 0.010..0.120; цель […], попадание […]
```

Смещения `MaxDistance` (0x2C) и `BaseDispersion` (0x3C) заведены в карту и
таблицу резолвера — обновляются автоматически.

### Картофель, часть вторая: нативные настройки Unity (26 сентября 2026)

Жалоба «я думал, текстуры станут мыльными» справедлива: URP-ассет масштабом
рендера размывает кадр целиком, а **качество текстур** живёт не в нём, а в
нативном `QualitySettings` внутри `libunity.so`. Символов там нет, поэтому
смещения добыты разбором кода — но именно разбором: **в игру по-прежнему
пишутся только данные, ни одной инструкции не патчится**.

**Как добыто** (`tools/offsets/unity_quality.py`, запускается на любом новом
`libunity.so`):

1. в файле ищется строка регистрации ICall, например
   `UnityEngine.QualitySettings::set_globalTextureMipmapLimit`;
2. один проход дизассемблера по `.text` находит пару `adrp/add`, которая
   грузит адрес этой строки; рядом стоит `adr x1, <нативная функция>`;
3. в переходнике читается общий пролог (получение менеджера по индексу) и
   хвостовой `b` в настоящий сеттер;
4. из сеттера снимается раскладка: `ldrsw x8,[x0,#0x64]` (текущий уровень),
   `ldr x10,[x0,#0x20]` (массив уровней), `mov w9,#0x108` (шаг) и смещение
   поля из `ldr … [x8, #off]!`.

**Что получилось:**

```
таблица менеджеров: base + 0x17d3088, индекс QualitySettings: 11
уровень:  *(int32*)(qs + 0x64)      массив: *(void**)(qs + 0x20)   шаг: 0x108
поля уровня: мипы текстур +0x5c, LOD-смещение +0xa4, макс. LOD +0xa8,
             попиксельный свет +0x28, тени +0x3c, сглаживание +0x84
```

**Что пишет «Картофельная графика» теперь** (сверх URP-части):

| настройка | значение | эффект |
|---|---|---|
| `globalTextureMipmapLimit` | 2 | текстуры в **четверть** разрешения — то самое «мыло» и минус память |
| `maximumLODLevel` | 1 | самые детальные меши не используются |
| `lodBias` | 0.3 | дальние модели переключаются на простые раньше |
| `pixelLightCount` | 0 | без попиксельных источников |
| `antiAliasing` | 0 | без сглаживания |
| `shadowDistance` | 0 | без теней |

Оригиналы сохраняются и возвращаются при выключении тумблера, смене уровня
качества и отвязке от игры. Уровень проверяется по содержимому (мипы 0…8,
LOD-смещение 0.05…20) — в чужой объект не пишем.

**Важно про версионность.** Эти смещения зависят от версии Unity, а не от
сборки игры, поэтому автообновление по именам их не трогает (имён в libunity
нет вовсе). При смене движка достаточно перезапустить скрипт:
`python3 tools/offsets/unity_quality.py <новый libunity.so>` — он печатает
готовые строки для `game_offsets*.h`.

Оговорка по мипам: предел применяется к текстурам при загрузке, поэтому уже
загруженные подтянутся по мере стриминга (при перемещении по миру), а новые
грузятся сразу пониженными.

#### Лог 26.09 23:27: четыре разбора

**Оружие — `отказ=2` при ИЗВЕСТНОМ build-id.** В логе
`id=d85e63cd…f307` — это релизная сборка, которая ЕСТЬ в `kLayouts`, но не
сошлись сторожевые инструкции. Сторожа появились как защита от старых
инструкционных патчей и заодно подтверждали раскладку — а раскладку теперь
подтверждают живые метаданные. Поэтому расхождение сторожей больше не
приговор: блок переходит в generic-режим, если вся цепочка смещений живая
(и отказывает, как и раньше, если нет).

**Сайлент.** Промахи посчитаны: `выстрелов 16, попаданий 15 (93%)`, медиана
0.2–0.4 м — вблизи он работает. А вот строка баллистики объясняет дальние
промахи:

```
оружие бьёт на 200 м, конус 0.000..20.000
```

Дальность в порядке (200 м), но **конус разброса до 20** — на дистанции пуля
уходит куда угодно. То есть лечится не сайлентом, а «без разброса» из блока
«Оружие» — который как раз и молчал из-за сторожей (см. выше). Плюс нулевая
точка попадания теперь честно считается «попадания нет», а не промахом в 90 м.

**Картофель.** URP-часть работает, запись видна в логе:
`сейчас в ассете scale=0.40 (просили 0.40, у игры было 0.54) тени=0 м`.
А нативной части (мипы текстур) в логе нет вовсе — значит
`potato_quality_level()` каждый раз возвращал ноль и молча выходил. Добавлена
диагностика: теперь пишется, на каком шаге рвётся (нет менеджера по таблице,
уровень вне диапазона, нет массива, неправдоподобные поля) и какие адреса
прочитаны. По следующему логу станет ясно, та ли у устройства `libunity.so`,
что лежит в репозитории.

**Тач-аим — перебор с гашением.** Прошлая правка расширяла мёртвую зону до
70% амплитуды (потолок 2.5°) и резала шаг до 0.35 с медленным
восстановлением: качка замедлилась, но вместе с ней замедлился и сам аим.
Пересол снят (0.35 амплитуды, потолок 0.6°, трим не ниже 0.6 и возвращается
за 0.3 с), а вместо силы ограничен **темп**: следующий шаг выдаётся только
после того, как камера отозвалась на предыдущий (или через четыре такта, если
отклика нет вовсе). Наш цикл идёт 60 раз в секунду, игра рисует 30–40 — лишние
заказы между кадрами игры и удваивали петлевое усиление.

### Сайлент бьёт ровно в точку: нулевой разброс без патчей (26 сентября 2026)

Замер показал, что сайлент целится верно (15 попаданий из 16 вблизи, медиана
промаха 0.2–0.4 м), а дальние выстрелы уходят «в никуда» из-за разброса
самого оружия — в логе `конус 0.000..20.000`. Ось стоит точно в кости, но
пуля летит внутри конуса, и на дистанции это гарантированный промах.

Разброс — это ДАННЫЕ (`DispersionConfig.minDispersion/maxDispersion` плюс
кэш базового разброса в калькуляторе оружия), и их уже умеет обнулять блок
«Оружие». Поэтому сайлент теперь просит тот же механизм:

* пока сайлент ведёт цель, поднимается флаг «нужен нулевой разброс», и
  `weapons.tick()` получает `no_spread = тумблер ИЛИ сайлент`;
* используются те же проверки, что и у тумблера: живая цепочка смещений,
  совпадение классов по имени, перекрёстные ссылки, сохранение оригиналов и
  возврат при выключении;
* флаг «липкий» — держится полторы секунды после потери цели: цель
  теряется и находится по нескольку раз в секунду, и без этого запись в
  память игры молотила бы туда-обратно каждый кадр.

Патчей кода по-прежнему нет: это те же записи в объектные данные, что делает
сам движок, когда игрок меняет настройки оружия.

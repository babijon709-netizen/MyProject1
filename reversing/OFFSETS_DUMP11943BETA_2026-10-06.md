# dumpbeta11943 — оффсеты бета-клиента, 2026-10-06

## Входной артефакт

| Параметр | Значение |
|---|---|
| Google Drive file id | `1sbMoeTQd75XNl5GeOUAsOwx4RC4Ak3_6` |
| Архив | `dumpbeta11943.7z` |
| Размер | `59141385` байт |
| MD5 | `aafa289fb92d2b69f11dbdd0e2c480da` |
| SHA-256 | `1b16e9aaa21f57d24028cc32118c94dc453d94a44ac4de1944690fb477170a4e` |

Состав архива — те же четыре файла, что и у релиза:

| Файл | Размер | SHA-256 |
|---|---:|---|
| `dump.cs` | `140708438` | `a395c7e0925a00450547d4f190ce67c90c038f9684b1b6bb9ac9b1e9d72a292b` |
| `il2cpp.h` | `222230419` | `e4df6b3ad1f52e01626f008afada674fdb8456b6ee6c23cac1087308790631e5` |
| `libil2cpp.so` | `236097008` | `bf1a535fe000c0f602ad02bab299b5cb6362d3a2374e9cd162f64857fd27dc43` |
| `script.json` | `237966647` | `a3267165b574eb9296de915b7ff257eaf2587479b51b566c160d756769457541` |

Метадата — тот же формат: il2cpp_dump.py, v39, `XOR 0xa5c3f19d +
перестановка троек`, `31791` типов, `285133` методов, `140588` полей,
`RVA` у методов прямо в `dump.cs`.

## Сверка с релизом dump11945

Сверялся каждый класс-якорь из `game_offsets.h` (способ: парсинг
`il2cpp.h` в карту struct->поля, сравнение позиционно по смещению/типу и по
имени; обфусцированные классы — через цепочки типов от PlayerManager и
фингерпринты полей). Результат: **managed-layouts двух билдов совпадают во
всех используемых классах** (PlayerManager 76+76 полей, PlayerEventHandler
76/76, FPHitscan, FPWeaponBase, FPManager, FPObject, FPMelee/FPTool,
RaycastManager, KCC, GenericVitals, MineableObject + оба hitstreak +
HitMarkerItem, LootObject, ItemPickup, PlayerWeapon incl. SyncVar-хвост
0x150/0x158/0x160, view (WeaponBase 0x48/WeaponPiece 0x50/Transform 0x60),
GKO (hitObject 0x18/RaycastHit 0x48), TOD (_Cycle_ 0x40, 67/67 полей),
здания pmp/pmK (Entity 0x68, Health 0x98) и пр.).

Единственное расхождение managed-layout:

| Константа | Релиз | Бета | Причина |
|---|---:|---:|---|
| `INVDATA_PLAYER_MODEL_INFO` | `0x20` | `0x18` | в релиз перед playerModelInfo вставлено поле `Vale...` (+0x18); в бете его нет |

Native Unity-оффсеты (Camera/Transform/GameObject) у беты не проверялись —
в архиве нет `libunity.so`; оставлены релизные значения из `game_offsets.h`.
Enum'ы (`Oxide.FPTool.ToolPurpose` 1/2/4, `ServerPlayersAnalytics.EntityType`
0..25 incl. Rat=25) — идентичны, общие в `game_offsets.h`.

## TypeInfo inner-slots

Метод как у релиза: `adrp/ldr outer` -> `R_AARCH64_RELATIVE addend` ->
**inner slot** -> `ldr [klass,#0xB8]`. В код идут addend'ы. Каждый слот
подтверждён двумя независимыми способами, оба откалиброваны на релизном
ELF (там ответы известны из `game_offsets.h`):

1. **Сигнатуры statics-доступа** (register-level dataflow: slot -> klass ->
   +0xB8 -> offsets). У релиза и беты сигнатуры совпадают по составу и
   почти по счётчикам, что отличает свой класс от «соседнего шума»
   (релизный шум 0xdcc3928 у PM и 0xdcc8888 у TOD имеет ровно те же
   сигнатуры, что бетовые 0xdd72690 и 0xdd77708).
2. **YQ::.cctor@0x682c2b8** — прямой аналог ZD::.cctor@0x6831a8c: читает
   outer `0xda1dfa0` и пишет statics+#0 (static List) через свой слот.

| Константа | Релиз | Бета | outer в бете |
|---|---:|---:|---|
| `PLAYER_MANAGER_TYPEINFO_RVA` | `0xDCC45F8` | `0xDD732B8` | `0xda1fdf0` |
| `PLAYER_LOOP_HELPER_TYPEINFO_RVA` | `0xDCC45C8` | `0xDD73280` | `0xda1ec60` |
| `GAME_CONTROLLER_TYPEINFO_RVA` | `0xDCBF840` | `0xDD6E5F8` | `0xda1df30` |
| `NETWORK_CLIENT_TYPEINFO_RVA` | `0xDCC3860` | `0xDD725C8` | `0xda1df18` |
| TOD inner slot (для окна скана) | `0xdcc9b90` | `0xdd789d8` | `0xda1dfa0` |
| `TOD_SCAN_RVA_BEGIN` | `0xDCC0000` | `0xDD70000` | — |
| `TOD_SCAN_RVA_END` | `0xDD80000` | `0xDE30000` | — |

Окно TOD сохраняет форму релиза: `0xC0000` байт, слот на `+0x89d8`
(~35 шагов по 1 КБ). Пары PM↔PLH в бете соседние (`0xDD732B8` и `0xDD73280`,
Δ=0x38), как и в релизе (Δ=0x30).

## Имена дампа (хлебные крошки)

| Назначение | dump11945 | dumpbeta11943 |
|---|---|---|
| PlayerEventHandler | `pLh` | `maY` |
| activity (Run/Crouch/Jump/Aim) | `pLx` | `mar` |
| activity (ост.) | `pLv` | `maU` |
| Nicklabel | `Xq` | `dL` |
| VoicePlayer | `prF` | `mgn` |
| TOD_Sky | `ZD` | `YQ` |
| weapon view ref chain (PlayerWeapon) | `lL`/`lQ` | `eR`/`eI` |
| GKO/GUI обёртки raycast | `pLD`/`pLV` | `maZ`/`mao` |

## Применение

`game_offsets.h` — релизный набор (как был), `game_offsets_beta.h` —
полный бетовый (значения этой таблицы), `game_offsets_active.h` — inline-
переменные `go_active::X` с `UseRelease()/UseBeta()`. Выбор — экран
«Версия игры» перед экраном способа чтения (`version_select.h`), выбор
хранится в `CfgDir()/.gversion` (`release`/`beta`). Имя пакета у клиентов
одинаковое, автоопределения нет — только ручной выбор; поэтому экран
показывается на каждом запуске, а прошлый выбор подсвечен рамкой.

Инструменты этой сессии (не в git): `/home/user/tools/{parse_layout.py,
cmp_structs.py, pos_cmp.py, typeinfo_slots.py, statics_flow.py}`.
Raw-дистрибутивы дампов в git не добавляются.

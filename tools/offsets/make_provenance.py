#!/usr/bin/env python3
"""Пересобрать tools/offsets/PROVENANCE.md из карты и заголовка.

    python3 tools/offsets/make_provenance.py [--build NEW] [--prev OLD]

Документ — объяснение к `offsets_map.json`: не «какое число стоит», а «откуда
оно взялось и как его найти заново». Таблицы полей и рантайм-констант
генерируются из карты, поэтому после `update_offsets.py --apply` файл стоит
пересобрать (числа и имена полей в нём должны совпадать с заголовком).

Разделы, которых в карте нет (имена классов, функциональные якоря, ловушки
билда), живут в этом скрипте как текст — правь их здесь же.
"""
import argparse
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MAP = os.path.join(HERE, 'offsets_map.json')
HEADER = os.path.join(ROOT, 'jni', 'src', 'game_offsets.h')
OUT = os.path.join(HERE, 'PROVENANCE.md')

# Отпечатки TypeInfo: как отличить верный слот от чужого (проверено на паре
# дампов 89e0b63 -> 62a8534; см. журнал в OFFSETS_UPDATE.md).
RVA_FINGERPRINT = {
    'PLAYER_MANAGER_TYPEINFO_RVA': '5 обращений, статик-поля `[0x1A0x1]`',
    'GAME_CONTROLLER_TYPEINFO_RVA':
        '1 обращение, статик-полей нет — **верхний кандидат здесь всегда чужой**',
    'NETWORK_CLIENT_TYPEINFO_RVA': '6 обращений, статик-полей нет',
}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', default='62a8534', help='коммит/билд текущего дампа')
    ap.add_argument('--prev', default='89e0b63', help='коммит/билд предыдущего дампа')
    ap.add_argument('--map', default=MAP)
    ap.add_argument('--out', default=OUT)
    a = ap.parse_args()

    d = json.load(open(a.map, encoding='utf-8'), object_pairs_hook=collections.OrderedDict)
    src = open(HEADER, encoding='utf-8').read()
    vals = {m.group(1): m.group(2) for m in
            re.finditer(r'inline constexpr std::uint64_t (\w+)\s*=\s*(0x[0-9A-Fa-f]+)', src)}

    by_struct = collections.OrderedDict()
    for k, v in d.items():
        if v.get('kind') == 'field':
            by_struct.setdefault(v['struct'], []).append(k)
    runtime = [(k, v) for k, v in d.items() if v.get('kind') == 'runtime']
    rva = [(k, v) for k, v in d.items() if v.get('kind') == 'typeinfo_rva']
    nf = sum(len(v) for v in by_struct.values())

    o = []
    w = o.append
    w('# Откуда берётся каждый оффсет (PROVENANCE)')
    w('')
    w(f'Живой срез для билда `{a.build}` (предыдущий — `{a.prev}`). Дампы лежат в')
    w('корне репозитория: `dump.7z` (dump.cs, il2cpp.h, script.json), `libil2cpp.7z`')
    w('и `libunity.7z`; распаковка — `bash tools/offsets/extract_dumps.sh <dir> [git-ref]`.')
    b = d.get('_build') or {}
    if b:
        w('')
        w('### Отпечаток текущего набора дампов')
        w('')
        w('Первое, что делается с новыми дампами: сравнить эти цифры. Совпала')
        w('версия движка — класс C (рантайм/ABI) трогать не нужно, едут только поля')
        w('и RVA. Не совпала — пересчитывать всё, включая раскладку нативных')
        w('объектов Unity.')
        w('')
        w('| что | значение |')
        w('|---|---|')
        w(f"| версия Unity | **{b.get('unity','?')}** (из строк libil2cpp.so/libunity.so) |")
        w(f"| версия метаданных | v{b.get('metadata_version','?')} (шапка dump.cs) |")
        counts = b.get('counts')
        if counts:
            w('| таблицы | типов %d, методов %d, полей %d |' %
              (counts['types'], counts['methods'], counts['fields']))
        for f in ('dump.cs', 'il2cpp.h', 'script.json', 'libil2cpp.so', 'libunity.so'):
            if f in b:
                w('| `%s` | %d байт, sha1 `%s…` |' % (f, b[f]['size'], b[f]['sha1_12']))
        w('')
    w('')
    w('Машиночитаемый источник истины — `tools/offsets/offsets_map.json`')
    w(f'({len(d)} записей); этот файл — его человеческое объяснение: не «какое число')
    w('стоит», а «откуда оно взялось и как его найти заново». Значения здесь')
    w('совпадают с `jni/src/game_offsets.h`; при расхождении верить заголовку и')
    w('карте, а этот файл пересобрать (`make_provenance.py`).')
    w('')
    w('Константы делятся на три класса — и обновляются они по-разному:')
    w('')
    w('| класс | сколько | откуда | уезжает ли каждый билд | чем проверяется |')
    w('|---|---|---|---|---|')
    w(f'| **A. Смещения полей** | {nf} | раскладка структур в `il2cpp.h` (`/* 0xNN */`) | '
      f'да, если в класс добавили/убрали поле | `update_offsets.py` автоматически |')
    w(f'| **B. `*_TYPEINFO_RVA`** | {len(rva)} | слоты глобальных `Il2CppClass*` в '
      f'`.data.rel.ro` `libil2cpp.so` | **да, всегда** | `typeinfo_rva.py` + отпечаток '
      f'со старого дампа |')
    w(f'| **C. Рантайм/ABI** | {len(runtime)} | раскладка Unity/IL2CPP-объектов, не '
      f'выводится из `dump.cs` | только при смене версии Unity/IL2CPP | вручную '
      f'(дизасм паттернов), см. §C |')
    w('')
    w('Отдельно — не константы, но тоже привязано к билду: имена классов (сверяются')
    w('в рантайме со строкой `Il2CppClass.name`) и окно скана `TOD_SCAN_RVA_*`.')
    w('')
    w('---')
    w('')
    w('## A. Смещения полей (`il2cpp.h`)')
    w('')
    w('Как читать: `структура` — имя из `il2cpp.h` (`<Класс>_Fields`), `поле` — имя в')
    w(f'**этом** билде (`{a.build}`). Обфусцированные имена ротируют каждый билд')
    w('(`MoW`→`lzD`), поэтому искать поле надо по смещению+типу, а не по имени;')
    w('скрипт так и делает (сначала имя, если оно читаемое, иначе позиционное')
    w('выравнивание последовательности типов старой структуры на новую).')
    w('')
    w('Часть констант живёт не в именованной структуре, а в классе ПО указателю')
    w('из неё (`PlayerManager.playerEventHandler` → `Aim`), и имя того класса')
    w('тоже ротирует (`DqO`→`Gum`). В карте такие записи несут `via` — путь от')
    w('стабильной структуры по читаемым именам полей; `update_offsets.py`')
    w('разрешает его в имена структур обоих дампов и проверяет константу как')
    w('обычное поле. Без `via` проверка вырождается в «у `PlayerManager` есть')
    w('поле по 0x268» — это верно всегда, и сдвиг внутри самого хендлера')
    w('проходит молча. Так и вышло в билде `62a8534`: туда вставили `KnockDoor`')
    w('(0x188), всё после него уехало на +8, `Aim` сместился 0x268→0x270, а по')
    w('0x268 встал `Jump` — переключатель «Только в прицеле» читал флаг прыжка.')
    w('')
    for st, keys in by_struct.items():
        cls = d[keys[0]].get('class') or st
        w(f'### `{st}`  ({cls})')
        w('')
        w(f'| константа | offset | поле в билде `{a.build}` | тип |')
        w('|---|---|---|---|')
        for k in sorted(keys, key=lambda x: d[x]['offset']):
            e = d[k]
            w(f"| `{k}` | {e['offset']:#x} | `{e.get('field','?')}` | `{e.get('type','?')}` |")
        w('')
    w('---')
    w('')
    w('## B. `*_TYPEINFO_RVA` — слоты глобальных `Il2CppClass*`')
    w('')
    w('Ридер не может найти класс по имени (метадата-таблицы в `.so` нет,')
    w('`ScriptMetadata` в `script.json` пустой), поэтому читает готовый указатель из')
    w('`.data.rel.ro`: `rd_ptr(base + RVA)` → `Il2CppClass*`, дальше имя класса')
    w('(`+0x10`/`+0x18`) для контроля и `static_fields` (`+0xB8`).')
    w('')
    w('| константа | RVA | класс | отпечаток (как отличить от чужого слота) |')
    w('|---|---|---|---|')
    for k, v in rva:
        w(f"| `{k}` | {vals.get(k,'?')} | `{v.get('class')}` | {RVA_FINGERPRINT.get(k,'')} |")
    w('')
    w('Как найти заново (оба прогона обязательны):')
    w('')
    w('```bash')
    w('# 1) старый дамп: скрипт обязан воспроизвести значения, которые сейчас в git')
    w('python3 tools/offsets/typeinfo_rva.py --so <old>/libil2cpp.so --script <old>/script.json \\')
    w('        --methods 3000 --top 6 Oxide.PlayerManager Oxide.GameControllerBase Mirror.NetworkClient')
    w('# 2) новый дамп: берём кандидат С ТЕМ ЖЕ отпечатком, а не верхнего')
    w('python3 tools/offsets/typeinfo_rva.py --so <new>/libil2cpp.so --script <new>/script.json \\')
    w('        --methods 3000 --top 6 Oxide.PlayerManager Oxide.GameControllerBase Mirror.NetworkClient')
    w('```')
    w('')
    w(f'`--methods 3000` обязателен: на 400 методах у `PlayerManager` в билде `{a.build}`')
    w('не находится ни одного кандидата. `update_offsets.py` делает оба прогона сам и')
    w('подбирает по отпечатку; если отпечаток не совпал — предупреждает, и тогда')
    w('решать вручную.')
    w('')
    w('---')
    w('')
    w('## C. Рантайм/ABI — не из `dump.cs`')
    w('')
    w('Раскладка нативных объектов Unity и служебных структур IL2CPP. В дампе игры её')
    w('нет, поэтому значения получены дизассемблером/рантаймом и меняются только при')
    w('смене версии Unity. `update_offsets.py` их не проверяет (в карте `kind:')
    w('runtime`) — после крупного апдейта движка сверять руками.')
    w('')
    w('| константа | значение | что это | где искать в дампах |')
    w('|---|---|---|---|')
    for k, v in sorted(runtime, key=lambda kv: (kv[1].get('src', {}).get('file', ''), kv[0])):
        src = v.get('src', {})
        where = src.get('file', '?')
        if src.get('how'):
            where += ' — ' + src['how']
        w(f"| `{k}` | {v['offset']:#x} | {v.get('note','')} | {where} |")
    w('')
    w('Дешёвая перепроверка ABI по новому `libil2cpp.so` (без запуска игры):')
    w('')
    w('* `Il2CppClass.static_fields == 0xB8` — в любом методе, читающем статики:')
    w('  `adrp/ldr` слота → `ldr xA,[xM]` → `ldr xB,[xA,#0xb8]`. В билде')
    w(f'  `{a.build}`: `OreHitstreaksMarker.Update` 0x786eb9c.')
    w('* `klass->interfaceOffsets == 0xB0`, `interface_offsets_count == 0x12E` —')
    w(f'  `MineableObject` 0x656781c/0x6567828 (билд `{a.build}`).')
    w('* `Il2CppArray`: длина 0x18, первый элемент 0x20; `List<T>`: `_items` 0x10,')
    w('  `_size` 0x18 — см. любой перебор коллекции в коде игры.')
    w('')
    w('Где что лежит, коротко: `libunity.so` — нативные Camera/Transform/')
    w('Component/GameObject (Camera ищется по геттерам `*_Injected`, этоякорь')
    w('на весь класс); `libil2cpp.so` — ABI служебных структур и слоты TypeInfo;')
    w('`il2cpp.h` — поля управляемых классов и порядок полей generic-обёрток.')
    w('Смещения generic-обёрток (`List<T>`, `Dictionary<K,V>`, `SyncVar<T>`,')
    w('`AsyncReactiveProperty<T>`) в дампе отсутствуют совсем: они считаются')
    w('порядком полей плюс заголовок объекта 0x10, а у элементов массива')
    w('структур заголовка нет — отсюда `DICT_ENTRY_STRIDE` 0x18, а не 0x28.')
    w('')
    w('**Расхождение, которое надо помнить при обновлении:** `RAYCASTHIT_COLLIDER`')
    w('(0x28) в текущем дампе имеет тип `UnityEngine_EntityId_o`, а не')
    w('`UnityEngine_Collider_o*`, как было записано в карте раньше. Смещение')
    w('верное, тип в карте исправлен, но код (§ поиск «перекрытие собственным')
    w('камнем» в `game.cpp`) всё ещё сравнивает содержимое 0x28 как указатель на')
    w('коллайдер. При обновлении проверить, не стала ли эта проверка всегда')
    w('ложной — тогда руду снова начнёт «перекрывать» своим же камнем.')
    w('')
    w('---')
    w('')
    w('## D. Имена классов, которые сверяются в рантайме')
    w('')
    w('Код читает `Il2CppClass.name` (`+0x10`) и сравнивает со строкой, поэтому имя')
    w('класса — тоже часть «оффсетов». Эти имена в текущем билде на месте:')
    w('')
    w('* `MineableObjectExtension_OreHitstreaks`, `MineableObjectExtension_TreeHitstreaks`')
    w('  (цель автофарма; `kFarmExtOreClass` / `kFarmExtTreeClass` в `game.cpp`),')
    w('  а также `…_OreHitstreaksMarker`, `…_HitMarkerItem`;')
    w('* префикс `Mineable` (`MineableStone`/`MineableTree`/…), `LootObject`,')
    w('  `ItemPickup`, `LootDestroyable`, `PumpkinTrick` — маркеры ESP;')
    w('* `PlayerManager`, `GameControllerBase`, `NetworkClient`, `NetworkIdentity`,')
    w('  `List\\`1` — служебные проверки.')
    w('')
    w('Базовый интерфейс экстеншенов ротировал `JE`→`dk` — код его не использует')
    w('(коллекция `MINEABLE_EXTENSIONS` перебирается с определением формы на лету:')
    w('имя класса начинается с `List\\`1` → список, иначе массив).')
    w('')
    w('Сверка одной командой (должно быть непусто для каждого имени):')
    w('')
    w('```bash')
    w("grep -c '^public class .*MineableObjectExtension_OreHitstreaks' <new>/dump.cs")
    w('```')
    w('')
    w(f'Между `{a.prev}` и `{a.build}` из имён, которые знает код, исчез только `DVL` —')
    w('и это не класс в нашем смысле, а строка-лейбл в таблице `kWeaponNames`.')
    w('')
    w('---')
    w('')
    w('## E. Чек-лист обновления (порядок на новые дампы)')
    w('')
    w('1. Распаковать новые дампы: `bash tools/offsets/extract_dumps.sh /tmp/new`')
    w('   (`bash`, не `sh` — в скрипте `set -o pipefail`).')
    w('2. Сличить отпечаток: `python3 tools/offsets/build_map.py /tmp/new --check`.')
    w('   Версия движка та же → класс C не трогаем; отличается → пересчитываем')
    w('   `libunity.so`/ABI руками (§C).')
    w('3. Сверить поля: `python3 tools/offsets/verify_map.py /tmp/new`. Выданный')
    w('   список расхождений и есть рабочий список на обновление.')
    w('4. `python3 tools/offsets/update_offsets.py --old <старый коммит> --apply` —')
    w('   сам пересчитает поля и RVA; без `--apply` он только печатает diff.')
    w('5. Имена классов (§D) сверить grep-ом по новому `dump.cs`.')
    w('6. Пересобрать карту и этот файл:')
    w('   `python3 tools/offsets/build_map.py /tmp/new` и')
    w('   `python3 tools/offsets/make_provenance.py --build <NEW> --prev <OLD>`.')
    w('7. Стенды: `sh tools/offsets/run.sh && sh tools/syntax/check.sh`.')
    w('8. Только после сборки GitHub Actions — просить проверку на устройстве.')
    w('   Первым делом проверять камеру: если PlayerManager/GameController уехал,')
    w('   сыпется вся цепочка до камеры, и это выглядит как «камера не найдена»')
    w('   и «потеря MouseLook» — симптомы, которые легко принять за баг логики.')
    w('')
    w('---')
    w('')
    w('## E. Функциональные якоря (для ручной перепроверки семантики)')
    w('')
    w('Сам код НИ ОДНУ функцию игры не вызывает — только читает память, поэтому VA')
    w('методов не являются зависимостью. Они нужны, чтобы после апдейта убедиться,')
    w('что поле по-прежнему означает то же самое (а не просто стоит на том же')
    w('смещении). Имена методов обфусцированы и ротируют; якорь ищется по')
    w('сигнатуре (`dump.cs`: класс + типы параметров) и по характерным константам в')
    w('дизассемблерации.')
    w('')
    w(f'| роль | билд `{a.prev}` | билд `{a.build}` | как узнать |')
    w('|---|---|---|---|')
    w('| `MineableObject`: ленивое заполнение `MINEABLE_EXTENSIONS` (0xE8) | `cik` 0x648e6ec '
      '| 0x65677d0 | `ldr x?,[x?,#0xe8]!` → `cbnz` → `GetComponents<dk>` → `str x0,[x22]` |')
    w('| руда: перечитывает живой маркер 0x30 и узел 0x40 каждый кадр | `Update` 0x772bfb4 '
      '| `Update` 0x786b260 | имя `Update` не обфусцировано |')
    w('| руда: запись живого маркера 0x30 | `gir`/`gil` 0x772d1ec/0x772a548 '
      '| 0x786b03c (`str x0,[x19,#0x30]`) | единственный `str` не-нуля в 0x30 |')
    w('| руда: обнуление 0x30 (крестик потух) | `giq` 0x7729c90 '
      '| 5 мест: 0x7868cc8, 0x7869a78, 0x786a56c, 0x786aed8, 0x786b600 | `str xzr,[x19,#0x30]` |')
    w('| маркер руды: таймер жизни 0x58, порог **15.0**, затем `Destroy` | `Update` 0x772f3bc '
      '| `Update` 0x786eb40 | `ldr s?,[x19,#0x58]` … `fmov s?,#15.0` |')
    w('| дерево: проверка попадания (серия засчитана) | `giL` 0x77313dc (+`rRS`/`DMU`/`Dne`/`DfW`) '
      '| `bool(Vector3)`: 0x786fa30, 0x786fbd0, 0x787331c, 0x78740e0, 0x7874280 '
      '| читают 0x50 (живой клон) и 0x88 (точка на коре) |')
    w('| дерево: пересчёт сегмента 0x88/0xA4 | `giJ`/`WO`/`Dd` '
      '| `str x8,[x19,#0x88]` в 0x7870630, 0x78712ec, 0x7872abc | парная запись 0x88 и 0xA4 |')
    w('| дерево: сброс счётчика серии 0x48 | `giD` 0x7731258 | `str wzr,[x19,#0x48]` (3 места) | — |')
    w('| `TOD_Sky` (время суток): класс с `Cycle` на 0x40 | `IY` | `UV` '
      "| `grep -n 'TOD_CycleParameters_o\\* Cycle' il2cpp.h` |")
    w('')
    w('Инструменты ручной сверки: `tools/offsets/il2cpp_layout.py`')
    w('(`diff`/`show`/`find`; список отслеживаемых структур — `TRACKED`, крестик там')
    w('есть) и `tools/offsets/typeinfo_rva.py`. Для дизассемблерации с аннотацией')
    w('имён полей нужен одноразовый хелпер (`capstone` + `il2cpp.h`): VA→смещение в')
    w('файле считается по PT_LOAD-сегментам ELF, а не хардкодом (в билде `89e0b63`')
    w('дельта была 0x4000, в `62a8534` — другая).')
    w('')
    w('---')
    w('')
    w('## F. Что вообще не из дампов')
    w('')
    w('* **Таблица item-id → название оружия** (`weapon_label_for_item_id` в')
    w('  `game.cpp`): снята с устройства (`items.txt`, `/storage/emulated/0/benzhack`),')
    w('  id приходит из синхронизируемого `WeaponPiece.Number`. Из дампов не')
    w('  проверяется; при смене базы предметов — снять заново. Промах не фатален:')
    w('  неизвестный id уходит на фолбэк по имени префаба.')
    w('* **Окно скана `TOD_SCAN_RVA_BEGIN/END`**: диапазон `.data.rel.ro`, а не поле.')
    w('  Пересчёт — комментарий в `game_offsets.h`; контроль — оба кандидата')
    w('  `TOD_Sky` из `typeinfo_rva.py` обязаны попасть в окно, а число слотов в нём')
    w(f'  должно быть ~80 тысяч (в `{a.prev}` их там 82 154, в `{a.build}` — 82 167;')
    w('  35 слотов в старом окне нового билда — верный признак, что окно протухло).')
    w('* **Нативные смещения `Camera`** (§3.1 `OFFSETS_UPDATE.md`) — из `libunity.so`,')
    w('  обновляются отдельно и только при смене Unity.')
    w('')
    w('## G. Поколения обфусцированных имён (чтобы не пугаться diff\'а)')
    w('')
    w('| билд | backing-поля | пример полей крестика |')
    w('|---|---|---|')
    w(f'| до `{a.prev}` | `_ukT_k__BackingField` | `MoW`, `MTn`, `MTQ` |')
    w(f'| `{a.prev}` | `_Q*_k__BackingField` | `MoW`, `MTn`, `MTQ` |')
    w(f'| `{a.build}` | `_L*_k__BackingField` | `lzD`, `lHe`, `lHC` |')
    w('')
    w('Смещения при этом не двигались — ротировали только имена. Важно: читаемые')
    w('имена полей (`hitstreakIndex`, `meshRenderer`, `lifetime`, `mark`, …) между')
    w(f'соседними билдами сохраняются, НО в `{a.build}` впервые обфусцировалась часть')
    w('читаемых полей `PlayerWeapon`, поэтому страховка «читаемое имя не ротирует» в')
    w('`update_offsets.py` ослаблена: `_XXXX_k__BackingField` считается')
    w('обфусцированным именем и в проверке поколения не участвует.')

    open(a.out, 'w', encoding='utf-8').write('\n'.join(o) + '\n')
    print(f'{os.path.relpath(a.out, ROOT)}: {len(o)} строк '
          f'({nf} полей, {len(rva)} RVA, {len(runtime)} runtime)')
    return 0


if __name__ == '__main__':
    sys.exit(main())

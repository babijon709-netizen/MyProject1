#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Генерирует jni/src/offset_resolver_table.inc — таблицу автообновления оффсетов.

Резолвер на устройстве (jni/src/offset_resolver.cpp) читает IL2CPP-метаданные
живого процесса: находит Il2CppClass* по имени класса и для каждого нужного
поля берёт смещение из FieldInfo (имя поля -> смещение). Таблица говорит ему,
КАКУЮ константу (слот в go_active) каким (класс, поле) заполнять.

Источник — tools/offsets/offsets_map.json (карта «константа -> класс/поле»,
которой пользуется и beta_offsets.py). Берём только записи вида «поле класса»
со стабильными именами:

  * классы, чьи имена РОТИРУЮТ обфускатором (помечены в карте «имя
    ротирует», а также короткие обфусцированные имена), пропускаем — такие
    классы ищутся не по имени, а через живые объекты (ручная работа);
  * поля короче 4 символов («BQo», «LLI», ...) тоже ротируются — пропускаем;
  * статик-поля («класс.static») помечаем отдельно: их смещение считается
    от блока статик (klass->static_fields), но живёт в том же FieldInfo.

Имя класса для поиска — как правило, последний токен имени из дампа:
у классов с пространством имён («Oxide_FPManager») в рантайме имя короткое
(«FPManager», пространство имён «Oxide»). Коллизии коротких имён разруливает
сам резолвер: класс принимается только если в нём нашлись поля. НО: часть
классов носит подчёркивание ПРЯМО В ИМЕНИ (не разделитель пространства
имён) — у них в рантайме имя полное, и резолвер обязан искать целиком:

  * MineableObjectExtension_OreHitstreaks (и …Marker, …TreeHitstreaks,
    …HitMarkerItem) — game_farm.cpp сверяет живые объекты побайтово по
    полному имени «MineableObjectExtension_OreHitstreaks»;
  * TOD_CycleParameters — ассет TOD, имя класса с префиксом «TOD_».

Такие классы перечислены в KEEP_FULL и в .inc попадают с полным именем.
Лог 24.09 показал цену обрезки: резолвер искал «HitMarkerItem»/
«OreHitstreaks»/«CycleParameters» и не находил их — 5 классов и 12 полей
оставались на вшитых значениях после апдейта игры.

Пятый столбец записи — сигнатура ТИПА поля из карты: как искать поле,
когда его ИМЯ ротировано обфускатором (_BYK_k__BackingField -> _CQx_...).
Примитив (f4/b1/u4/i4/str), «любой ссылочный» (obj), единственное ссылочное
поле класса (obj1) или класс по имени (c:Item). obj1 применяется только когда
дампы подтверждают, что в целевом классе такая ссылка одна; резолвер также
отказывается от него при нескольких runtime-кандидатах.

    python3 tools/offsets/gen_resolver_table.py          # переписать .inc
"""

import json
import os
import re
import sys

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
MAP = os.path.join(REPO, 'tools/offsets/offsets_map.json')
ACTIVE = os.path.join(REPO, 'jni/src/game_offsets_active.inc')
OUT = os.path.join(REPO, 'jni/src/offset_resolver_table.inc')

# Классы с ротацией имени: их нельзя искать по имени в рантайме.
SKIP_CLASSES = {'eDt'}

# Классы, у которых подчёркивание — ЧАСТЬ имени, а не разделитель пространства
# имён: в рантайме они живут под полным именем, обрезать его нельзя.
KEEP_FULL = {
    'MineableObjectExtension_OreHitstreaks',
    'MineableObjectExtension_OreHitstreaksMarker',
    'MineableObjectExtension_TreeHitstreaks',
    'MineableObjectExtension_HitMarkerItem',
    'TOD_CycleParameters',
}


def go_field_names():
    names = set()
    rx = re.compile(r'^GO_FIELD\(([A-Za-z0-9_]+),')
    with open(ACTIVE, encoding='utf-8') as f:
        for line in f:
            m = rx.match(line.strip())
            if m:
                names.add(m.group(1))
    return names


def short_class_name(cls):
    """Полное имя класса из карты -> имя в рантайме. Обычно последний токен;
    классы из KEEP_FULL сохраняют имя целиком (см. шапку)."""
    cls = cls.removesuffix('.static')
    cls = cls.replace('.', '_')
    if cls in KEEP_FULL:
        return cls
    return cls.split('_')[-1]


# Тип поля из карты -> сигнатура для резолвера:
#   ''       — подсказки нет (поле ищется только по имени);
#   f4/b1/…  — примитив: сверяется enum типа (Il2CppType, биты 16..23);
#   str      — System.String;
#   obj      — любой ссылочный тип (класс/массив/объект);
#   obj1     — специальная подсказка: ровно одно ссылочное поле в классе;
#   c:Имя    — класс/структура: сверяется klassIndex типа с byval_arg класса.
def type_sig(entry):
    # Narrow resolver-only hints are separate from the dump type consumed by
    # beta_offsets.py. obj1 means one unique reference field in the target class.
    resolver_type = entry.get('resolver_type')
    if resolver_type:
        return resolver_type if resolver_type == 'obj1' else ''
    t = (entry.get('type') or '').strip()
    if not t:
        return ''
    # Тип с пояснением в скобках («euD_o* / Reo_o* (имя ротируется...)») —
    # подсказки нет: класс меняется от билда к билду.
    if '(' in t:
        return ''
    prim = {
        'float': 'f4',
        'bool': 'b1',
        'uint32_t': 'u4',
        'uint': 'u4',
        'int32_t': 'i4',
        'int': 'i4',
        'int16_t': 'i2',
        'System_String_o*': 'str',
        'System.String*': 'str',
    }
    if t in prim:
        return prim[t]
    if t in ('Il2CppObject*', 'System_Object_o*'):
        return 'obj'
    m = re.match(r'^(?:[A-Za-z0-9]+_)*([A-Za-z0-9]+)_(?:o\*?)$', t)
    if m:
        return 'c:' + m.group(1)
    return ''


def rot_name(entry):
    """Текущее ротированное имя класса из строки класса.

    В карте класс с ротирующимся именем описан как
    «PlayerEventHandler (имя ротирует: DqO -> Gum -> brc -> eDg)» — последний
    токен после «->» и есть имя в текущем билде (оно же суффикс struct:
    eDg_Fields). Для «eDt» ротации в строке нет — имя и так ротированное.
    Нужно резолверу: тип поля бывает «c:eDw», то есть ссылкой на ДРУГОЙ
    ротированный класс, и искать такой тип в кэше классов бессмысленно.
    """
    cls = entry.get('class') or ''
    if 'ротирует' not in cls:
        return cls.split(' ')[0] if cls else ''
    tail = cls.rsplit('->', 1)[-1].strip()
    return tail.rstrip(')').strip()


def via_hops(entry):
    """Цепочка «как добраться до класса с ротированным именем» из карты.

    В карте: via = [{'struct': 'Oxide_PlayerManager_Fields', 'field':
    'playerEventHandler'}, {'field': 'Aim'}] — из PlayerManager через поле
    playerEventHandler приходим в класс обработчика событий, а через его поле
    Aim — в класс активности. Возврат: [стартовый_класс, поле, поле2] (поле2
    может быть ''), либо [] если цепочки нет или она неполная.
    """
    via = entry.get('via')
    if not via:
        return []
    hops = []
    for hop in via:
        struct = hop.get('struct')
        if struct:
            # 'Oxide_PlayerManager_Fields' -> 'PlayerManager'
            name = struct[:-len('_Fields')] if struct.endswith('_Fields') else struct
            hops.append(short_class_name(name))
        hops.append(hop.get('field') or '')
    if len(hops) == 2:
        hops.append('')
    if len(hops) != 3 or not hops[0] or not hops[1]:
        return []
    return hops


def main():
    omap = json.load(open(MAP, encoding='utf-8'))
    known = go_field_names()

    emit = []       # (const, short, field, is_static, sig)
    emit_via = []   # (const, [стартовый_класс, поле, поле2], field, is_static, sig)
    skipped = []    # (const, причина)
    for const, entry in sorted(omap.items()):
        if entry.get('kind') != 'field':
            continue
        if const not in known:
            skipped.append((const, 'нет в go_active'))
            continue
        cls = entry.get('class') or ''
        note = entry.get('note', '')
        # Цепочка «через объект»: класс с ротированным именем (или вообще без
        # имени в карте) достигается как ТИП поля известного класса. По типу
        # резолвер вычисляет индекс определения типа, а по индексу находит
        # живой класс в памяти — имя ему не нужно (см. phase_kid).
        hops = via_hops(entry)
        via_ok = bool(hops) and (not cls or 'ротирует' in cls or cls in SKIP_CLASSES)
        if not cls:
            if via_ok:
                emit_via.append((const, hops, entry.get('field') or '', False,
                                 type_sig(entry), rot_name(entry)))
                continue
            skipped.append((const, 'класс не указан (через объект)'))
            continue
        # Ротируется ИМЯ КЛАССА — только это делает класс ненаходимым по имени.
        # Ротируется имя ПОЛЯ — не беда: поле ищется по типу (пятый столбец),
        # поэтому упоминание «ротирует» в note нас не останавливает.
        if 'ротирует' in cls or cls in SKIP_CLASSES:
            if via_ok:
                emit_via.append((const, hops, entry.get('field') or '',
                                 cls.endswith('.static'), type_sig(entry),
                                 rot_name(entry)))
            else:
                skipped.append((const, 'имя класса ротируется'))
            continue
        if 'луч' in cls:  # «результат луча» — не класс
            skipped.append((const, 'не класс'))
            continue
        is_static = cls.endswith('.static')
        short = short_class_name(cls)
        if len(short) <= 3 and short not in ('KCC',):
            skipped.append((const, f'короткое имя класса {short!r}'))
            continue
        field = entry.get('field') or ''
        sig = type_sig(entry)
        # Поле с коротким/ротированным именем («XeK», «id») РАНЬШЕ пропускалось
        # совсем, и его константа навсегда оставалась на вшитом значении. Лог
        # 24.09 показал цену: OREHS_MARKER / TREEHS_MARKER / TREEHS_SPOT_A /
        # MINEABLE_EXTENSIONS / OREMARK_AGE — от них зависит крестик автофарма,
        # и после апдейта игры фарм молча бил по корпусу вместо крестика.
        # Теперь такое поле попадает в таблицу, если известен его ТИП: имя
        # тогда не нужно, резолвер берёт смещение по типу (см.
        # resolve_type_group в offset_resolver.cpp). Без типа искать
        # действительно нечем — только по имени, а оно ротировано.
        if not field:
            skipped.append((const, 'поле не задано'))
            continue
        if (len(field) <= 3 or 'k__BackingField' in field) and not sig:
            skipped.append((const, f'поле {field!r} ротируется/короткое, типа нет'))
            continue
        emit.append((const, short, field, is_static, sig))

    # Дедупликация: одна и та же (класс, поле) может питать несколько констант —
    # это нормально, оставляем как есть (каждая константа получает своё значение).
    lines = []
    lines.append('// Таблица сгенерирована tools/offsets/gen_resolver_table.py из')
    lines.append('// tools/offsets/offsets_map.json — руками не править.')
    lines.append('// Формат: OR_FIELD(константа_go_active, "ИмяКласса", "имяПоля", статик, "тип")')
    lines.append('// "тип" — сигнатура для поиска поля по ТИПУ, когда его имя ротировано:')
    lines.append('//   "" нет подсказки; f4/b1/u4/i4 примитивы; str System.String;')
    lines.append('//   obj любой ссылочный; c:Имя — класс/структура с этим коротким именем.')
    for const, short, field, is_static, sig in emit:
        lines.append(f'OR_FIELD({const}, "{short}", "{field}", {str(is_static).lower()}, "{sig}")')
    # У записей без класса ротированное имя неизвестно, но цепочка та же, что
    # у соседей (EVENT_HANDLER_LAST_HIT_POINT идёт тем же путём, что и остальные
    # PlayerEventHandler) — берём имя у них, чтобы и «c:eDw» разрешался.
    rot_by_chain = {}
    for _c, _h, _f, _s, _g, _r in emit_via:
        if _r:
            rot_by_chain.setdefault(tuple(_h[1:]), _r)
    emit_via = [(c, h, f, s, g, r or rot_by_chain.get(tuple(h[1:]), ''))
                for c, h, f, s, g, r in emit_via]

    if emit_via:
        lines.append('')
        lines.append('// Классы с ротированным именем. OR_VIA(константа, "стартовыйКласс",')
        lines.append('// "поле", "поле2" (пусто — цепочка из одного шага), "искомоеПоле", статик, "тип",')
        lines.append('// "ротИмяЦелевогоКласса") — последнее резолвер использует, когда тип поля')
        lines.append('// сам ссылается на ротированный класс («c:eDw»): сверять не с чем, поэтому')
        lines.append('// сопоставляем имя из карты с kidx, который дала цепочка.')
        for const, hops, field, is_static, sig, rot in emit_via:
            lines.append(f'OR_VIA({const}, "{hops[0]}", "{hops[1]}", "{hops[2]}", '
                         f'"{field}", {str(is_static).lower()}, "{sig}", "{rot}")')

    header = '\n'.join(lines) + '\n'
    old = open(OUT, encoding='utf-8').read() if os.path.exists(OUT) else ''
    if old != header:
        open(OUT, 'w', encoding='utf-8').write(header)
        print(f'записано {OUT}: {len(emit)} записей')
    else:
        print(f'{OUT} не изменился: {len(emit)} записей')

    print(f'через цепочку (имена классов ротируются): {len(emit_via)}')
    print(f'пропущено {len(skipped)}:')
    from collections import Counter
    by_reason = Counter(r for _, r in skipped)
    for reason, n in by_reason.most_common():
        print(f'  {n:3d}  {reason}')
    for const, reason in skipped:
        print(f'    {const}: {reason}')


if __name__ == '__main__':
    main()

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

Имя класса для поиска — последний токен имени из дампа: вложенные классы
в il2cpp зовутся внутренним именем («MineableObjectExtension.TreeHitstreaks»
в рантайме имеет имя «TreeHitstreaks»), поэтому полное имя с точками/
подчёркиваниями сводится к последнему куску. Коллизии коротких имён
разруливает сам резолвер: класс принимается только если в нём нашлись поля.

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
    """Полное имя класса из карты -> короткое имя в рантайме (последний токен)."""
    cls = cls.removesuffix('.static')
    return cls.replace('.', '_').split('_')[-1]


def main():
    omap = json.load(open(MAP, encoding='utf-8'))
    known = go_field_names()

    emit = []       # (const, short, field, is_static)
    skipped = []    # (const, причина)
    for const, entry in sorted(omap.items()):
        if entry.get('kind') != 'field':
            continue
        if const not in known:
            skipped.append((const, 'нет в go_active'))
            continue
        cls = entry.get('class') or ''
        note = entry.get('note', '')
        if not cls:
            skipped.append((const, 'класс не указан (через объект)'))
            continue
        if 'ротирует' in cls or 'ротирует' in note or cls in SKIP_CLASSES:
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
        if not field or len(field) <= 3:
            skipped.append((const, f'поле {field!r} ротируется/короткое'))
            continue
        emit.append((const, short, field, is_static))

    # Дедупликация: одна и та же (класс, поле) может питать несколько констант —
    # это нормально, оставляем как есть (каждая константа получает своё значение).
    lines = []
    lines.append('// Таблица сгенерирована tools/offsets/gen_resolver_table.py из')
    lines.append('// tools/offsets/offsets_map.json — руками не править.')
    lines.append('// Формат: OR_FIELD(константа_go_active, "КороткоеИмяКласса", "имяПоля", статик)')
    for const, short, field, is_static in emit:
        lines.append(f'OR_FIELD({const}, "{short}", "{field}", {"true" if is_static else "false"})')

    header = '\n'.join(lines) + '\n'
    old = open(OUT, encoding='utf-8').read() if os.path.exists(OUT) else ''
    if old != header:
        open(OUT, 'w', encoding='utf-8').write(header)
        print(f'записано {OUT}: {len(emit)} записей')
    else:
        print(f'{OUT} не изменился: {len(emit)} записей')

    print(f'пропущено {len(skipped)}:')
    from collections import Counter
    by_reason = Counter(r for _, r in skipped)
    for reason, n in by_reason.most_common():
        print(f'  {n:3d}  {reason}')
    for const, reason in skipped:
        print(f'    {const}: {reason}')


if __name__ == '__main__':
    main()

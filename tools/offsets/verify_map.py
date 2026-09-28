#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Сверить карту оффсетов с дампом.

    python3 tools/offsets/verify_map.py /tmp/dump
    python3 tools/offsets/verify_map.py /tmp/dump --map tools/offsets/offsets_map.json

Первый шаг любой работы с оффсетами: после обновления игры дампы меняются, и
прежде чем пересчитывать константы, надо понять, что именно уехало. Скрипт
читает il2cpp.h (структуры и смещения полей в комментариях /* 0xNN */) и для
каждой записи карты класса «field» проверяет:

  * структура с таким именем есть в дампе;
  * поле с таким именем в ней есть (имя обфусцировано и ротирует от билда
    к билду — поэтому имя сверяется, но при расхождении это НЕ всегда ошибка);
  * смещение поля совпадает с записанным в карте;
  * цепочка via (структура, до которой надо дойти по полям от стабильной)
    разрешается в то же имя структуры.

Тип поля тоже печатается: при обновлении игры по типу проще всего найти
поле заново, когда имя переехало.

Выход — список расхождений. Для обновления это и есть рабочий список.
"""

import argparse
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

STRUCT_RE = re.compile(r'^struct\s+(\w+?)(?:\s*:\s*(\w+))?\s*\{')
FIELD_RE = re.compile(r'^\t(.+?);\s*/\*\s*(0x[0-9A-Fa-f]+)\s*\*/')
VIA_RE = re.compile(r'^(\w[\w:<>,\s\*]*?)\s+(\w+);')


def target_struct(ftype):
    """Имя структуры, куда ведёт тип поля: «Foo_o*» -> «Foo_Fields»."""
    t = ftype.strip().rstrip('*').strip()
    if t.endswith('_o'):
        return t[:-2] + '_Fields'
    if t.endswith('_Fields') or t.endswith('_c'):
        return t
    return ''


def load_needed(path):
    """Имена структур, которые стоит вынимать из дампа (карта + цепочки via)."""
    d = json.load(open(path, encoding='utf-8'), object_pairs_hook=collections.OrderedDict)
    names = set()
    for v in d.values():
        if v.get('struct'):
            names.add(v['struct'])
        for step in v.get('via', []) or []:
            if step.get('struct'):
                names.add(step['struct'])
    return d, names


def scan_structs(header, needed):
    """Один проход по il2cpp.h: собрать только нужные структуры.

    Файл — сотни мегабайт, поэтому в память кладём только то, что спрашивают:
    имя структуры -> {имя поля: (смещение, тип)}.
    """
    out = {}
    cur = None
    parent = None
    with open(header, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            if cur is None:
                m = STRUCT_RE.match(line)
                if m and m.group(1) in needed:
                    cur, parent = m.group(1), m.group(2)
                    out[cur] = {'__parent__': parent or '', 'fields': {}}
                continue
            if line.startswith('};'):
                cur = None
                continue
            m = FIELD_RE.match(line)
            if m:
                decl, off = m.group(1), int(m.group(2), 16)
                fm = re.match(r'^(.*?)(\w+)$', decl.strip())
                if not fm:
                    continue
                ftype = fm.group(1).strip()
                fname = fm.group(2)
                out[cur]['fields'][fname] = (off, ftype, decl.strip())
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dumpdir', nargs='?', default='/tmp/dump', help='каталог с распакованными дампами')
    ap.add_argument('--map', default=os.path.join(HERE, 'offsets_map.json'))
    ap.add_argument('--show-ok', action='store_true', help='печатать и совпавшие записи')
    a = ap.parse_args()

    header = os.path.join(a.dumpdir, 'il2cpp.h')
    if not os.path.exists(header):
        sys.exit('нет %s — распакуй дампы: bash tools/offsets/extract_dumps.sh %s' % (header, a.dumpdir))

    d, needed = load_needed(a.map)
    print('дамп: %s' % a.dumpdir)
    print('записей в карте: %d, структур интересно: %d' % (len(d), len(needed)))
    structs = scan_structs(header, needed)
    print('найдено структур в дампе: %d из %d' % (len(structs), len(needed)))
    missing_structs = sorted(needed - set(structs))
    if missing_structs:
        print('НЕ НАЙДЕНЫ структуры: %s' % ', '.join(missing_structs))

    bad = []
    ok = 0
    for name, v in d.items():
        if v.get('kind') != 'field':
            continue
        st = v.get('struct')
        fld = v.get('field')
        want = v.get('offset')
        if st not in structs:
            bad.append((name, 'нет структуры %s' % st))
            continue
        fs = structs[st]['fields']
        if fld not in fs:
            # имя переехало: попробовать найти поле по смещению и типу —
            # для обновления это и есть подсказка, куда оно уехало.
            same_off = [f for f, (o, t, _s) in fs.items() if o == want]
            hint = (' на 0x%X теперь %s' % (want, ', '.join(
                '%s: %s' % (f, fs[f][1]) for f in same_off))) if same_off else ''
            bad.append((name, 'нет поля %s в %s%s' % (fld, st, hint)))
            continue
        off, ftype, decl = fs[fld]
        if off != want:
            bad.append((name, 'смещение %s.%s = 0x%X, в карте 0x%X' % (st, fld, off, want)))
            continue
        if v.get('type') and v['type'].rstrip('* ') not in ftype:
            bad.append((name, 'тип %s.%s = «%s», в карте «%s»' % (st, fld, ftype, v['type'])))
            continue
        ok += 1
        if a.show_ok:
            print('  ок  %-34s %s.%s = 0x%X (%s)' % (name, st, fld, off, ftype))

    # цепочки via: идём от стабильной структуры по читаемым именам полей,
    # каждый шаг — из той структуры, куда привёл предыдущий
    for name, v in d.items():
        chain = v.get('via') or []
        if not chain:
            continue
        cur = None
        path = []
        for step in chain:
            st, fld = step.get('struct'), step.get('field')
            if st:
                cur = st
            if not cur or not fld:
                continue
            if cur not in structs:
                bad.append((name, 'via: в дампе нет структуры %s' % cur))
                break
            fs = structs[cur]['fields']
            if fld not in fs:
                bad.append((name, 'via: нет поля %s.%s' % (cur, fld)))
                break
            off, ftype = fs[fld][0], fs[fld][1]
            path.append('%s.%s=0x%X' % (cur, fld, off))
            nxt = target_struct(ftype)
            if nxt:
                cur = nxt
            elif not step.get('struct'):
                break          # дальше ведёт неназванный тип — проверить нечем
        # конец цепочки должен попадать в структуру из карты
        last = [s for s in chain if s.get('struct')]
        if cur and v.get('struct') and cur != v['struct'] and path:
            # сравниваем ТОЛЬКО последний шаг: промежуточные ведут в свои классы
            last_type = None
            st, fld = (last[-1].get('struct'), last[-1].get('field')) if last else (None, None)
            if st in structs and fld in structs[st]['fields']:
                last_type = target_struct(structs[st]['fields'][fld][1])
            if last_type and last_type != v['struct']:
                bad.append((name, 'via: %s ведёт в «%s», а в карте «%s»'
                            % (' -> '.join(path), last_type, v['struct'])))
            elif not last_type:
                ok += 1
                print('  ок  %-34s via %s' % (name, ' -> '.join(path)))
            else:
                ok += 1
                print('  ок  %-34s via %s -> %s' % (name, ' -> '.join(path), v['struct']))
        elif path:
            ok += 1
            print('  ок  %-34s via %s' % (name, ' -> '.join(path)))

    print('---')
    print('полей совпало: %d, расхождений: %d' % (ok, len(bad)))
    for name, why in bad:
        print('  ✗ %-34s %s' % (name, why))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

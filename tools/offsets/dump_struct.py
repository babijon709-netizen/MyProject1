#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Показать раскладку структуры из дампа.

    python3 tools/offsets/dump_struct.py Oxide_PlayerManager_Fields
    python3 tools/offsets/dump_struct.py Oxide_PlayerManager_Fields /tmp/dump
    python3 tools/offsets/dump_struct.py /Oxide_Player   # по части имени — список

Печатает поля структуры в том виде, как они лежат в il2cpp.h: тип, имя и
смещение из комментария /* 0xNN */. Если структура наследуется, показывает
родителя — его поля идут первыми и их смещения видны в его же структуре.

Это кирпич, из которого собирается всё остальное: когда после обновления игры
имя обфусцированного поля переехало, новое имя находится здесь — по смещению
и типу (порядок полей в классе обычно сохраняется).
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STRUCT_RE = re.compile(r'^struct\s+(\w+?)(?:\s*:\s*(\w+))?\s*\{')
FIELD_RE = re.compile(r'^\t(.+?);\s*(?:/\*\s*(0x[0-9A-Fa-f]+)\s*\*/)?')


def find(header, name, limit=6):
    """Распечатать структуру (или все подходящие по части имени, если имя с '/')."""
    partial = name.startswith('/')
    pat = name[1:] if partial else name
    shown = 0
    cur = None
    parent = None
    buf = []
    with open(header, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            if cur is None:
                m = STRUCT_RE.match(line)
                if m and ((partial and pat in m.group(1)) or (not partial and m.group(1) == pat)):
                    cur, parent = m.group(1), m.group(2)
                    buf = []
                continue
            if line.startswith('};'):
                print('struct %s%s {' % (cur, ' : ' + parent if parent else ''))
                for b in buf:
                    print(b)
                print('};')
                print()
                shown += 1
                cur = None
                if shown >= limit:
                    return shown
                continue
            m = FIELD_RE.match(line)
            if m:
                decl, off = m.group(1).rstrip(), m.group(2)
                if '(' in decl:      # методы — не поля
                    continue
                buf.append('\t%-70s %s' % (decl, ('/* %s */' % off) if off else ''))
    return shown


def main():
    args = [a for a in sys.argv[1:]]
    dump = '/tmp/dump'
    if args and not args[0].startswith('/') and os.path.isdir(args[-1]):
        dump = args.pop()
    elif len(args) > 1 and os.path.isdir(args[-1]):
        dump = args.pop()
    header = os.path.join(dump, 'il2cpp.h')
    if not os.path.exists(header):
        sys.exit('нет %s — распакуй дампы: bash tools/offsets/extract_dumps.sh %s' % (header, dump))
    if not args:
        sys.exit(__doc__)
    total = 0
    for name in args:
        total += find(header, name)
    if not total:
        print('ничего не найдено')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

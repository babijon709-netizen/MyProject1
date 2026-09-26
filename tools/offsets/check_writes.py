#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Каждое смещение, по которому чит ПИШЕТ в память игры, должно переживать апдейт.

    python3 tools/offsets/check_writes.py

Зачем. Читать по устаревшему смещению — это «функция молчит». Писать по
устаревшему — это запись мимо: в проверенный по имени объект, но не в то поле.
Поэтому правило такое: любое смещение, участвующее в записи, обязано быть либо

  * в таблице автообновления (jni/src/offset_resolver_table.inc) — тогда после
    апдейта игры резолвер найдёт поле по имени класса и поля;
  * либо в списке исключений ниже — с объяснением, почему автообновление к нему
    неприменимо (нативные структуры Unity, у которых нет имён в метаданных, и
    смещения, которые ищутся живой цепочкой прямо в рантайме).

Скрипт разбирает jni/src/game.cpp: находит записи вида
`wr_buf(<объект> + CONST, …)` и хелперы `setf/seti/setb(CONST, …)`, и сверяет
каждое имя. Новая функция, которая начнёт писать по вшитой константе мимо
таблицы, свалит проверку — это и нужно.
"""
import pathlib
import re
import sys

# Смещения, которым автообновление по именам неприменимо. Каждое — с причиной.
ALLOWED = {
    # Нативный Unity: структуры libunity.so, имён в IL2CPP-метаданных нет вовсе.
    # Зависят от версии движка, а не от сборки игры; пересчитываются скриптом
    # tools/offsets/unity_quality.py и защищены проверками правдоподобия.
    'UNITY_MANAGERS_RVA': 'нативный Unity (libunity.so)',
    'UNITY_QUALITY_MANAGER_IDX': 'нативный Unity (libunity.so)',
    'QUALITY_CURRENT_LEVEL': 'нативный Unity (libunity.so)',
    'QUALITY_LEVEL_ARRAY': 'нативный Unity (libunity.so)',
    'QUALITY_LEVEL_STRIDE': 'нативный Unity (libunity.so)',
    'QUALITY_TEXTURE_MIPMAP_LIMIT': 'нативный Unity (libunity.so)',
    'QUALITY_LOD_BIAS': 'нативный Unity (libunity.so)',
    'QUALITY_MAX_LOD_LEVEL': 'нативный Unity (libunity.so)',
    'QUALITY_PIXEL_LIGHT_COUNT': 'нативный Unity (libunity.so)',
    'QUALITY_SHADOW_DISTANCE': 'нативный Unity (libunity.so)',
    'QUALITY_ANTI_ALIASING': 'нативный Unity (libunity.so)',
    # Нативная камера Unity: те же основания.
    'CAMERA_NEAR_CLIP': 'нативная Camera (libunity.so)',
    'CAMERA_VIEW_MATRIX': 'нативная Camera (libunity.so)',
    'CAMERA_VIEW_DIRTY': 'нативная Camera (libunity.so)',
    'CAMERA_PROJ_DIRTY': 'нативная Camera (libunity.so)',
    # Ось MouseLook: сам компонент ищется живой цепочкой каждый кадр, смещение
    # внутри него проверяется чтением (углы обязаны быть конечными и в пределах).
    'MOUSELOOK_ANGLES': 'компонент ищется живой цепочкой в рантайме',
}

WRITE_PATTERNS = [
    re.compile(r'wr_buf\([A-Za-z_][A-Za-z0-9_]* \+ ([A-Z][A-Z0-9_]+)'),
    re.compile(r'\bset[fib]\(([A-Z][A-Z0-9_]+)\s*,'),
    re.compile(r'wr_buf\(([A-Z][A-Z0-9_]+)\s*,'),
]


def main():
    root = pathlib.Path(__file__).resolve().parents[2]
    table = (root / 'jni/src/offset_resolver_table.inc').read_text(encoding='utf-8')
    in_table = set(re.findall(r'OR_(?:FIELD|VIA)\((\w+),', table))

    written = set()
    for name in ('jni/src/game.cpp', 'jni/src/game_markers.cpp', 'jni/src/game_farm.cpp'):
        text = (root / name).read_text(encoding='utf-8')
        for pattern in WRITE_PATTERNS:
            written.update(pattern.findall(text))

    missing = sorted(n for n in written if n not in in_table and n not in ALLOWED)
    covered = sorted(n for n in written if n in in_table)
    exempt = sorted(n for n in written if n in ALLOWED)

    print('смещений под записью: %d' % len(written))
    print('  под автообновлением: %d' % len(covered))
    print('  с обоснованным исключением: %d' % len(exempt))
    if missing:
        print('\nПРОВАЛ: пишем по смещениям, которых нет ни в таблице, ни в исключениях:')
        for n in missing:
            print('    %s' % n)
        print('\nДобавь поле в tools/offsets/offsets_map.json и перегенерируй таблицу\n'
              '(python3 tools/offsets/gen_resolver_table.py) либо внеси в ALLOWED с причиной.')
        return 1
    print('\nзаписи и автообновление: все проверки пройдены')
    return 0


if __name__ == '__main__':
    sys.exit(main())

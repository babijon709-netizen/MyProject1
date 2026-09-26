#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Смещения нативных настроек качества Unity (libunity.so) — без патчей кода.

    python3 tools/offsets/unity_quality.py /tmp/u/libunity.so

Зачем. «Картофельная графика» понижает качество ЗАПИСЬЮ В ДАННЫЕ. Часть
настроек живёт в URP-ассете (их находит обычное автообновление по именам
полей), а часть — в нативном QualitySettings внутри libunity.so: именно там
лежит предел мип-уровня текстур, то самое «мыло», ради которого всё и
затевалось. Символов в libunity нет, поэтому смещения добываются разбором
кода — но РАЗБОРОМ, а не патчем: в игру по-прежнему пишутся только данные.

Как оно устроено в Unity (проверено на libunity.so этой сборки):

    таблица менеджеров:  managers = base + MANAGERS_RVA
    QualitySettings:     qs = *(void**)(managers + 11*8)
    текущий уровень:     level  = *(int32*)(qs + 0x64)
    массив уровней:      levels = *(void**)(qs + 0x20)
    сам уровень:         L = levels + level * ELEM
    поле:                L + смещение из таблицы ниже

Скрипт это восстанавливает автоматически:

  1. ищет в файле строку регистрации ICall («UnityEngine.QualitySettings::
     set_lodBias» и т. п.);
  2. находит в коде пару adrp/add, которая грузит адрес этой строки — рядом
     стоит `adr x1, <нативная функция>` и переход в регистратор;
  3. в найденной функции читает общий пролог (получение синглтона) и
     хвостовой вызов реального сеттера;
  4. в сеттере снимает тройку «ldrsw x8,[x0,#lvl] / mov w9,#ELEM /
     ldr x10,[x0,#arr]» и итоговое смещение поля из `ldr … [x8, #off]!`.

Вывод — готовые константы для jni/src/game_offsets*.h. При обновлении Unity
скрипт достаточно перезапустить на новом libunity.so.
"""
import re
import sys

try:
    from elftools.elf.elffile import ELFFile
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
except ImportError:  # pragma: no cover - подсказка окружения
    sys.exit('нужны pyelftools и capstone: pip install pyelftools capstone')

SETTERS = [
    ('QUALITY_TEXTURE_MIPMAP_LIMIT', 'set_globalTextureMipmapLimit', 'int'),
    ('QUALITY_LOD_BIAS', 'set_lodBias', 'float'),
    ('QUALITY_MAX_LOD_LEVEL', 'set_maximumLODLevel', 'int'),
    ('QUALITY_PIXEL_LIGHT_COUNT', 'set_pixelLightCount', 'int'),
    ('QUALITY_SHADOW_DISTANCE', 'set_shadowDistance', 'float'),
    ('QUALITY_ANTI_ALIASING', 'set_antiAliasing', 'int'),
    ('QUALITY_VSYNC_COUNT', 'set_vSyncCount', 'int'),
    ('QUALITY_LOD_CROSS_FADE', 'set_enableLODCrossFade', 'int'),
]


def load(path):
    data = open(path, 'rb').read()
    elf = ELFFile(open(path, 'rb'))
    text = next(s for s in elf.iter_sections() if s.name == '.text')
    return data, text['sh_addr'], text['sh_offset'], text['sh_size']


def string_va(data, name):
    needle = ('UnityEngine.QualitySettings::' + name).encode() + b'\x00'
    i = data.find(needle)
    return i if i >= 0 else None   # в этих .so file offset == vaddr


def find_registrations(data, text_va, text_off, text_size, wanted):
    """Один проход по .text: адрес строки -> адрес нативной функции."""
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    re_adrp = re.compile(r'^(\w+), #(0x[0-9a-f]+)$')
    re_add = re.compile(r'^(\w+), (\w+), #(0x[0-9a-f]+)$')
    re_adr = re.compile(r'^(\w+), #(0x[0-9a-f]+)$')
    last, pending, out = {}, None, {}
    for addr, _sz, mn, op in md.disasm_lite(data[text_off:text_off + text_size], text_va):
        if mn == 'adrp':
            m = re_adrp.match(op)
            if m:
                last[m.group(1)] = int(m.group(2), 16)
        elif mn == 'add':
            m = re_add.match(op)
            if m and m.group(2) in last:
                va = last[m.group(2)] + int(m.group(3), 16)
                pending = va if va in wanted else None
        elif mn == 'adr' and pending is not None:
            m = re_adr.match(op)
            if m:
                out[pending] = int(m.group(2), 16)
                pending = None
    return out


def tail_target(data, text_va, text_off, va):
    """Хвостовой `b <addr>` в коротком переходнике ICall."""
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    for _a, _s, mn, op in md.disasm_lite(data[text_off + va - text_va:][:40], va):
        if mn == 'b':
            return int(op.lstrip('#'), 16)
    return None


def setter_layout(data, text_va, text_off, va):
    """Из тела сеттера: смещения уровня/массива, размер элемента и поле."""
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    level_off = arr_off = elem = field = None
    for _a, _s, mn, op in md.disasm_lite(data[text_off + va - text_va:][:80], va):
        m = re.match(r'^x8, \[x0, #(0x[0-9a-f]+)\]$', op)
        if mn == 'ldrsw' and m and level_off is None:
            level_off = int(m.group(1), 16)
        m = re.match(r'^w9, #(0x[0-9a-f]+)$', op)
        if mn == 'mov' and m and elem is None:
            elem = int(m.group(1), 16)
        m = re.match(r'^x10, \[x0, #(0x[0-9a-f]+)\]$', op)
        if mn == 'ldr' and m and arr_off is None:
            arr_off = int(m.group(1), 16)
        m = re.match(r'^[ws]\d+, \[x8, #(0x[0-9a-f]+)\]!$', op)
        if mn in ('ldr', 'ldrsw') and m and field is None:
            field = int(m.group(1), 16)
        if None not in (level_off, arr_off, elem, field):
            break
    return level_off, arr_off, elem, field


def managers_rva(data, text_va, text_off, getter_va):
    """Переходник `mov w0,#idx; b tbl` -> таблица менеджеров и индекс."""
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    idx = tbl = None
    for _a, _s, mn, op in md.disasm_lite(data[text_off + getter_va - text_va:][:16], getter_va):
        m = re.match(r'^w0, #(0x[0-9a-f]+)$', op)
        if mn == 'mov' and m:
            idx = int(m.group(1), 16)
        if mn == 'b':
            tbl = int(op.lstrip('#'), 16)
            break
    if tbl is None:
        return None, None
    page = off = None
    for _a, _s, mn, op in md.disasm_lite(data[text_off + tbl - text_va:][:24], tbl):
        m = re.match(r'^x8, #(0x[0-9a-f]+)$', op)
        if mn == 'adrp' and m:
            page = int(m.group(1), 16)
        m = re.match(r'^x8, x8, #(0x[0-9a-f]+)$', op)
        if mn == 'add' and m:
            off = int(m.group(1), 16)
            break
    return (page + off if page is not None and off is not None else None), idx


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'libunity.so'
    data, tva, toff, tsize = load(path)
    want = {}
    for const, name, kind in SETTERS:
        va = string_va(data, name)
        if va is None:
            print('  нет строки ICall: %s' % name)
            continue
        want[va] = (const, name, kind)
    regs = find_registrations(data, tva, toff, tsize, set(want))
    print('найдено регистраций: %d из %d\n' % (len(regs), len(want)))

    table = idx = None
    rows = []
    for sva, thunk in sorted(regs.items(), key=lambda kv: want[kv[0]][0]):
        const, name, kind = want[sva]
        setter = tail_target(data, tva, toff, thunk)
        if setter is None:
            continue
        lvl, arr, elem, field = setter_layout(data, tva, toff, setter)
        if table is None:
            # первый же переходник вызывает общий пролог «получить менеджер»
            md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
            for _a, _s, mn, op in md.disasm_lite(data[toff + thunk - tva:][:24], thunk):
                if mn == 'bl':
                    table, idx = managers_rva(data, tva, toff, int(op.lstrip('#'), 16))
                    break
        rows.append((const, name, kind, field, lvl, arr, elem))

    if table is not None:
        print('таблица менеджеров: base + 0x%x, индекс QualitySettings: %d' % (table, idx))
        print('адрес синглтона:    *(void**)(base + 0x%x)\n' % (table + idx * 8))
    for const, name, kind, field, lvl, arr, elem in rows:
        print('%-30s %-34s %-6s поле +0x%-4x  (уровень +0x%x, массив +0x%x, шаг 0x%x)'
              % (const, name, kind, field or 0, lvl or 0, arr or 0, elem or 0))

    print('\n--- для game_offsets*.h ---')
    if table is not None:
        print('inline constexpr std::uint64_t UNITY_MANAGERS_RVA        = 0x%x;' % table)
        print('inline constexpr std::uint64_t UNITY_QUALITY_MANAGER_IDX = %d;' % idx)
    if rows:
        _c, _n, _k, _f, lvl, arr, elem = rows[0]
        print('inline constexpr std::uint64_t QUALITY_CURRENT_LEVEL     = 0x%x;' % lvl)
        print('inline constexpr std::uint64_t QUALITY_LEVEL_ARRAY       = 0x%x;' % arr)
        print('inline constexpr std::uint64_t QUALITY_LEVEL_STRIDE      = 0x%x;' % elem)
    for const, _n, _k, field, _l, _a, _e in rows:
        print('inline constexpr std::uint64_t %-25s = 0x%x;' % (const, field or 0))


if __name__ == '__main__':
    main()

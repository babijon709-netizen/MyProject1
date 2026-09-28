#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Сверка раскладки UnityEngine.Camera в двух libunity.so — релиза и беты.

    python3 tools/offsets/unity_layout.py                  # оба .7z из корня репозитория
    python3 tools/offsets/unity_layout.py --release a.so --beta b.so

Зачем: нативные смещения Camera (§3.1 OFFSETS_UPDATE.md, kind: runtime в
offsets_map.json) не пересчитываются из дампов — они приходят из libunity.so и
общие для релиза и беты. Если бета собрана на другом Unity, её ESP поедет, а
заметить это по дампам нельзя: смещения просто не те поля.

Что делает (та же процедура, что описана в OFFSETS_UPDATE.md — «Проверка нового
libunity.so», только автоматически):

  1. достаёт libunity.so из архивов (или берёт переданные файлы) и печатает
     версию Unity в обоих — она и определяет раскладку;
  2. находит функцию пересборки матриц камеры по отпечатку в коде:
     читается dirty-байт view-кэша [#0x502], затем пишутся view-кэш [#0x70] и
     worldToClip [#0xf0] от одного и того же this-регистра. Печатает дизасм
     этой функции в обеих сборках и считает, сколько слов кода совпало
     (расходятся только адреса вызовов — сам код на месте);
  3. сравнивает счётчики обращений к каждому нашему смещению по всему .text,
     раздельно по типу приёмника (ldr_s, ldr_q, ldrb_w, add_x, ...). Раскладка
     не менялась, если числа совпадают; расхождения в единицы — шум от чужих
     классов с тем же смещением.

Итог печатается словами: «раскладка та же» или «ЕСТЬ ОТЛИЧИЯ, нужен ручной
перемер». Скрипт ничего не меняет в репозитории.
"""

import argparse
import collections
import os
import re
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')

# Смещения, которые знает чит (OFFSETS_UPDATE.md §3.1). Подписи — те, что
# используются в коде: 0x502/0x500 — флаги «кеш протух», 0x70/0xb0/0xf0 — кеши
# матриц, 0x20 — Transform камеры, 0x5c8 — матрица прошлого кадра.
WATCH = (0x20, 0x70, 0xb0, 0xf0, 0x170, 0x454, 0x458, 0x4e0, 0x500, 0x502, 0x5c8)

# Что считаем «нашей» функцией пересборки: (смещение чтения, смещение записи 1,
# смещение записи 2). Взято из дизасма: ldrb [this+0x502] -> add x0,this,#0x70
# и add x1,this,#0x70 / add x2,this,#0xf0 с тем же this.
FOOTPRINT = (0x502, 0x70, 0xf0, 0x20)


class Elf:
    """Минимум для чтения .text и версии Unity из строк."""

    def __init__(self, path):
        self.path = path
        self.b = open(path, 'rb').read()
        self.text, self.text_va = self._text()

    def _shdrs(self):
        shoff, = struct.unpack_from('<Q', self.b, 0x28)
        size, num, stridx = struct.unpack_from('<HHH', self.b, 0x3a)
        for i in range(num):
            yield struct.unpack_from('<IIQQQQ', self.b, shoff + i * size)
        self._stridx = stridx
        self._size = size
        self._num = num

    def _text(self):
        shoff, = struct.unpack_from('<Q', self.b, 0x28)
        size, num, stridx = struct.unpack_from('<HHH', self.b, 0x3a)
        hdrs = [struct.unpack_from('<IIQQQQ', self.b, shoff + i * size) for i in range(num)]
        st = hdrs[stridx][4]
        for name, _typ, _flags, addr, off, sz in hdrs:
            end = self.b.index(b'\0', st + name)
            if self.b[st + name:end] == b'.text':
                return self.b[off:off + sz], addr
        raise SystemExit('%s: нет секции .text' % self.path)

    def unity_version(self):
        m = re.findall(rb'[26]\d{3}\.\d+\.\d+[abfp]\d+', self.b)
        return max((x.decode() for x in m), key=len) if m else '?'

    def read(self, va, n):
        return self.text[va - self.text_va: va - self.text_va + n]


def unpack(archive):
    """libunity.so из .7z рядом с репозиторием."""
    import py7zr
    out = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'unity_layout_' +
                       os.path.basename(archive).replace('.7z', ''))
    os.makedirs(out, exist_ok=True)
    so = os.path.join(out, 'libunity.so')
    if not os.path.exists(so):
        with py7zr.SevenZipFile(os.path.join(REPO, archive)) as z:
            z.extractall(out)
    return so


def disasm(elf, va, n):
    """Ровно n инструкций от адреса — без «обрыва на мусоре»."""
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    out, pos = [], va
    while len(out) < n:
        got = False
        for ins in md.disasm(elf.read(pos, (n - len(out)) * 4 + 4), pos):
            out.append(ins)
            got = True
            if len(out) == n:
                break
        pos = (out[-1].address + 4) if out else pos + 4
        if not got:
            break
    return out


def _decode(elf, chunk=0x40000, detail=False):
    """Инструкции .text подряд.

    Capstone в python молча останавливается на первом слове, которое не
    разобрал (в .text попадаются литералы и выравнивание), поэтому после
    каждого обрыва продолжаем со следующего слова — иначе счётчики зависят от
    того, как нарезан кусок.
    """
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = detail
    base, end = elf.text_va, elf.text_va + len(elf.text)
    pos = base
    while pos < end:
        stop = min(pos + chunk, end)
        last = None
        for ins in md.disasm(elf.text[pos - base:stop - base], pos):
            last = ins
            yield ins
        pos = (last.address + 4) if last is not None else pos + 4


DIRTY_RE = re.compile(r'^w\d+, \[x(\d+), #0x%x\]$' % FOOTPRINT[0])
ADD_RE = re.compile(r'^x\d+, x(\d+), #0x([0-9a-f]+)$')


def find_rebuild(elf):
    """(начало функции, адрес чтения dirty) для пересборки матриц камеры.

    Отпечаток: читается dirty-байт view-кэша, ниже того же this пишутся кеши
    view и worldToClip. Ищем вперёд от чтения (в дизасме add'ы идут после него),
    а начало функции — назад, до границы (ret / sub sp / stp с pre-index).
    """
    recent = collections.deque(maxlen=48)
    this, left, view, clip, site = None, 0, False, False, 0
    for ins in _decode(elf):
        if this is not None and left:
            if ins.mnemonic == 'add':
                m = ADD_RE.match(ins.op_str)
                if m and m.group(1) == this:
                    v = int(m.group(2), 16)
                    if v == FOOTPRINT[1]:
                        view = True
                    if v == FOOTPRINT[2]:
                        clip = True
                    if view and clip:
                        fn = ins.address
                        for prev in reversed(recent):
                            if prev.mnemonic == 'ret':
                                fn = prev.address + 4
                                break
                            if prev.mnemonic in ('sub', 'add') and prev.op_str.startswith('sp, sp'):
                                fn = prev.address
                            elif prev.mnemonic in ('stp', 'str', 'stur') and '[sp, #-' in prev.op_str:
                                fn = prev.address
                        return fn, site
                left -= 1
                if left == 0:
                    this = None
        if ins.mnemonic == 'ldrb':
            m = DIRTY_RE.match(ins.op_str)
            if m:
                this, left, view, clip, site = m.group(1), 12, False, False, ins.address
        recent.append(ins)
    return None, None


DISP_RE = re.compile(r'\[x\d+(?:, #0x([0-9a-f]+))?\]')
IMM_RE = re.compile(r', #0x([0-9a-f]+)$')


def counts(elf):
    """(обращение, смещение) -> сколько раз, раздельно по типу приёмника."""
    out = {}
    for ins in _decode(elf):
        dst = ins.op_str[:1] if ins.op_str else '?'
        key = '%s_%s' % (ins.mnemonic, dst)
        m = DISP_RE.search(ins.op_str)
        if m and m.group(1) and int(m.group(1), 16) in WATCH:
            disp = int(m.group(1), 16)
            out[(key, disp)] = out.get((key, disp), 0) + 1
            continue
        m = IMM_RE.search(ins.op_str)
        if m and ins.mnemonic in ('add', 'sub') and int(m.group(1), 16) in WATCH:
            disp = int(m.group(1), 16)
            out[(key, disp)] = out.get((key, disp), 0) + 1
    return out


# Счётчики, по которым видно именно Camera: редкие скалярные чтения флагов и
# плоскостей отсечения. Считаем только обращения с базой-регистром (xN), то есть
# к полям объекта: `ldr s1, [sp, #0x170]` — это кадр стека чужой функции, и такая
# строка ломает сверку, если её не отсеять (в OFFSETS_UPDATE.md за 12 сентября
# числа 21/25/19/52 считались вместе с ними — здесь правило строже).
GATES = (('ldr_s', 0x454), ('ldr_s', 0x458), ('ldr_s', 0x4e0), ('ldr_s', 0x170),
         ('ldrb_w', 0x502), ('strb_w', 0x502))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--release', help='libunity.so релиза (по умолчанию из libunity.7z)')
    ap.add_argument('--beta', help='libunity.so беты (по умолчанию из libunity_beta.7z)')
    a = ap.parse_args()

    rel = Elf(a.release or unpack('libunity.7z'))
    bet = Elf(a.beta or unpack('libunity_beta.7z'))

    print('релиз: %s  Unity %s' % (os.path.basename(rel.path), rel.unity_version()))
    print('бета : %s  Unity %s' % (os.path.basename(bet.path), bet.unity_version()))
    if rel.unity_version() != bet.unity_version():
        print('\nВЕРСИИ UNITY РАЗНЫЕ — раскладку Camera нужно перемерять руками '
              '(§3.1 OFFSETS_UPDATE.md), копировать смещения нельзя.')
        return 1
    print('версия Unity одна и та же — раскладка обязана совпадать, проверяем код\n')

    bad = 0
    for tag, elf in (('релиз', rel), ('бета', bet)):
        fn, site = find_rebuild(elf)
        if fn is None:
            print('%s: функция пересборки матриц по отпечатку не найдена' % tag)
            bad += 1
            continue
        print('== %s: функция пересборки %#x (dirty-байт читается в %#x)' % (tag, fn, site))
        for ins in disasm(elf, fn, 22):
            print('   %#010x: %-8s %s' % (ins.address, ins.mnemonic, ins.op_str))
        print()

    rel_fn, _ = find_rebuild(rel)
    bet_fn, _ = find_rebuild(bet)

    # Кластер функций камеры: сама пересборка матриц и её соседи (копии кешей
    # projection/previousViewProjection). Если раскладка поехала, поехал бы он.
    if rel_fn and bet_fn:
        lo, n = 0x100, 0x2c0
        rels = disasm(rel, rel_fn - lo, n)
        bets = disasm(bet, bet_fn - lo, n)
        same, diff = 0, []
        for i, (a, b) in enumerate(zip(rels, bets)):
            if a.bytes == b.bytes:
                same += 1
                continue
            # Разные адреса в бете — это норма: бинарь пересобран, весь код и
            # rodata переехали. Такими словами и должны отличаться переходы,
            # adrp и следующая за ним пара add с тем же регистром.
            reloc = a.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz', 'adr', 'adrp')
            if not reloc and a.mnemonic == 'add' and a.op_str.startswith('x'):
                dst = a.op_str.split(',')[0].strip()
                for j in range(max(0, i - 3), i):
                    prev = rels[j]
                    if prev.mnemonic == 'adrp' and prev.op_str.split(',')[0].strip() == dst:
                        reloc = True
                        break
            if not reloc:
                diff.append(a)
        print('кластер камеры (%#x..%#x в релизе): %d слов; совпало байт-в-байт %d, '
              'переезд адресов (вызовы/adrp) %d, содержательных отличий %d'
              % (rel_fn - lo, rel_fn - lo + n * 4, n, same, n - same - len(diff), len(diff)))
        for ins in diff[:6]:
            print('   РАЗНОЕ: %#010x: %-8s %s' % (ins.address, ins.mnemonic, ins.op_str))
        if diff:
            bad += 1

    ca, cb = counts(rel), counts(bet)
    print('\n«редкие» счётчики (по ним видно именно Camera; релиз -> бета):')
    for key, disp in GATES:
        x, y = ca.get((key, disp), 0), cb.get((key, disp), 0)
        ok = (x == y)
        if not ok:
            bad += 1
        print('  %-8s %#7x: %5d -> %5d   %s' % (key, disp, x, y, 'OK' if ok else 'РАЗНОЕ'))

    print('\nостальные счётчики — справочно (в них сидят и чужие классы):')
    shown = 0
    for key, disp in sorted(set(ca) | set(cb), key=lambda k: (k[1], k[0])):
        if (key, disp) in GATES:
            continue
        x, y = ca.get((key, disp), 0), cb.get((key, disp), 0)
        if x != y:
            shown += 1
            print('  %-10s %#7x: %6d -> %6d' % (key, disp, x, y))
    print('  всего строк с расхождением: %d (шум: эти imm12 есть у всех классов)' % shown)

    if bad:
        print('\nИТОГ: есть отличия — нужен ручной перемер (§3.1 OFFSETS_UPDATE.md)')
        return 1
    print('\nИТОГ: кластер камеры и её счётчики совпали — раскладка Camera в бете та же,'
          '\n      нативные смещения §3.1 действительны без правок')
    return 0


if __name__ == '__main__':
    sys.exit(main())

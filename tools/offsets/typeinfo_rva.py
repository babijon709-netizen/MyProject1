#!/usr/bin/env python3
"""Поиск RVA глобальных `Il2CppClass*` (`..._TYPEINFO_RVA`) в libil2cpp.so.

Эти константы — слоты таблицы metadata-usage в `.data`; они **уезжают каждый
апдейт**, и наш ридер без них не стартует вообще (`resolve_runtime_player_list`
возвращает 0 → ESP пустой). В dump'е этой игры `script.json` содержит пустой
`ScriptMetadata`, поэтому адреса приходится доставать из бинаря.

Как: для методов самого класса ищем последовательность

    adrp xN, #page ; ldr xM, [xN, #off]     -> слот в .data.rel.ro
    R_AARCH64_RELATIVE addend этого слота    -> адрес глобальной Il2CppClass*
    ldr xA, [xM]                             -> сам Il2CppClass*
    ldr xB, [xA, #0xB8]                      -> klass->static_fields

Последний шаг — решающий фильтр: это ровно тот доступ, который делает наш код,
и он отсеивает TypeInfo чужих классов, которые метод тоже подгружает.

    python3 tools/offsets/typeinfo_rva.py --so libil2cpp.so --script script.json \\
        "Oxide.PlayerManager" "Oxide.GameControllerBase" "Mirror.NetworkClient"

Требует `pip install capstone`. Проверять всегда на СТАРОМ дампе тоже: там
ответ известен (значения из git-истории `game_offsets.h`) — если скрипт их
воспроизводит, новым значениям можно верить.
"""
import argparse
import bisect
import json
import struct
import sys
from array import array
from collections import Counter, defaultdict

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
from capstone.arm64 import ARM64_INS_ADRP, ARM64_INS_RET, ARM64_OP_IMM, ARM64_OP_MEM

R_AARCH64_RELATIVE = 1027
IL2CPP_CLASS_STATIC_FIELDS = 0xB8


class ELF:
    def __init__(self, path):
        self.f = open(path, 'rb')
        d = self.f.read(64)
        if d[:4] != b'\x7fELF' or d[4] != 2:
            raise SystemExit(f'{path}: не ELF64')
        phoff, = struct.unpack_from('<Q', d, 32)
        phentsize, phnum = struct.unpack_from('<HH', d, 54)
        self.f.seek(phoff)
        ph = self.f.read(phentsize * phnum)
        self.loads, dyn = [], None
        for i in range(phnum):
            p_type, _fl, p_off, p_va, _pa, p_fsz, _msz, _al = struct.unpack_from(
                '<IIQQQQQQ', ph, i * phentsize)
            if p_type == 1:
                self.loads.append((p_va, p_off, p_fsz))
            elif p_type == 2:
                dyn = (p_off, p_fsz)
        if dyn is None:
            raise SystemExit('нет PT_DYNAMIC')
        self.f.seek(dyn[0])
        db = self.f.read(dyn[1])
        tags = {}
        for i in range(0, len(db), 16):
            t, v = struct.unpack_from('<QQ', db, i)
            if t == 0:
                break
            tags[t] = v
        self.rela = (tags.get(7), tags.get(8))  # DT_RELA, DT_RELASZ

    def off(self, va):
        for va0, off0, sz in self.loads:
            if va0 <= va < va0 + sz:
                return off0 + (va - va0)
        return None

    def read(self, va, n):
        o = self.off(va)
        if o is None:
            return b''
        self.f.seek(o)
        return self.f.read(n)

    def relative_map(self):
        """-> (отсортированные слоты, их addend'ы) для R_AARCH64_RELATIVE."""
        addr, size = self.rela
        self.f.seek(self.off(addr))
        blob = self.f.read(size)
        tmp = []
        for i in range(0, size, 24):
            r_off, r_info, r_add = struct.unpack_from('<QQq', blob, i)
            if (r_info & 0xFFFFFFFF) == R_AARCH64_RELATIVE:
                tmp.append((r_off, r_add))
        tmp.sort()
        slots, adds = array('Q'), array('q')  # addend бывает отрицательным
        for a, b in tmp:
            slots.append(a)
            adds.append(b)
        return slots, adds


def scan(elf, slots, adds, method_addrs, maxins=400):
    """-> (Counter typeinfo->сколько раз читались статики,
           typeinfo -> Counter смещений прочитанных статик-полей)"""
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    hits, flds = Counter(), defaultdict(Counter)
    for ma in method_addrs:
        code = elf.read(ma, maxins * 4)
        if not code:
            continue
        pages, slotreg, classreg, sfreg = {}, {}, {}, {}
        for ins in md.disasm(code, ma):
            if ins.id == ARM64_INS_ADRP:
                o = ins.operands
                if len(o) == 2 and o[1].type == ARM64_OP_IMM:
                    d = o[0].reg
                    pages[d] = o[1].imm
                    for reg in (slotreg, classreg, sfreg):
                        reg.pop(d, None)
            elif ins.mnemonic in ('ldr', 'ldrb', 'ldrh') and len(ins.operands) == 2 \
                    and ins.operands[1].type == ARM64_OP_MEM:
                d, m = ins.operands[0].reg, ins.operands[1].mem
                if m.base in sfreg:
                    flds[sfreg[m.base]][m.disp] += 1
                for reg in (slotreg, classreg, sfreg):
                    reg.pop(d, None)
                if m.base in pages and m.disp and ins.mnemonic == 'ldr':
                    s = pages[m.base] + m.disp
                    i = bisect.bisect_left(slots, s)
                    if i < len(slots) and slots[i] == s:
                        slotreg[d] = adds[i]
                elif m.base in slotreg and m.disp == 0 and ins.mnemonic == 'ldr':
                    classreg[d] = slotreg[m.base]
                elif m.base in classreg and m.disp == IL2CPP_CLASS_STATIC_FIELDS \
                        and ins.mnemonic == 'ldr':
                    hits[classreg[m.base]] += 1
                    sfreg[d] = classreg[m.base]
            elif ins.id == ARM64_INS_RET:
                break
    return hits, flds


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--so', required=True, help='распакованный libil2cpp.so')
    p.add_argument('--script', required=True, help='script.json из дампа')
    p.add_argument('--methods', type=int, default=400, help='сколько методов класса сканировать')
    p.add_argument('--top', type=int, default=4)
    p.add_argument('classes', nargs='+', help='например Oxide.PlayerManager')
    a = p.parse_args()

    meth = json.load(open(a.script))['ScriptMethod']
    elf = ELF(a.so)
    slots, adds = elf.relative_map()
    print(f'{a.so}: R_AARCH64_RELATIVE = {len(slots)}')
    for cls in a.classes:
        pref = cls + '$$'
        addrs = [m['Address'] for m in meth if m['Name'].startswith(pref)][:a.methods]
        hits, flds = scan(elf, slots, adds, addrs)
        print(f'\n{cls}  (методов просканировано: {len(addrs)})')
        if not hits:
            print('   кандидатов нет — класс не читает свои статики; '
                  'попробуй --methods больше или другой класс-якорь')
        for v, c in hits.most_common(a.top):
            fl = ', '.join(f'0x{o:X}x{n}' for o, n in flds[v].most_common(5)) or '-'
            print(f'   0x{v:X}   обращений={c:<4} читаемые статик-поля[{fl}]')
        print('   ^ обычно верхний кандидат = TypeInfo самого класса, но не всегда:\n'
              '     прогони ту же команду на СТАРОМ дампе и сопоставь по позиции и\n'
              '     «отпечатку» (у GameControllerBase верный слот стабильно тот, что\n'
              '     без читаемых статик-полей, и его обгоняет чужой кандидат).')
    return 0


if __name__ == '__main__':
    sys.exit(main())

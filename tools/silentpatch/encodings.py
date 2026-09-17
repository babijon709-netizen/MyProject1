#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Проверка машинного кода патча сайлента (aarch64).

silent_patch.cpp собирает трамполин руками, инструкция за инструкцией, —
здесь эти коды разбираются обратно дизассемблером и сверяются с тем, что
написано в комментарии рядом с константой. Если какая-то инструкция
закодирована неверно, трамполин на устройстве уронит игру, поэтому
проверка обязательна и стоит в общих стойках.

Требуется capstone (pip install --target=.tools/capstone capstone). Если её
нет — проверка пропускается с предупреждением, стойка не падает.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'jni', 'src', 'silent_patch.cpp')

try:
    import capstone
except ImportError:
    print('capstone не установлен — проверка кода патча пропущена')
    sys.exit(0)

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
md.detail = True

# Константа + комментарий с ожидаемой инструкцией:
#   0xd10103ffu, // sub sp, sp, #0x40
#   constexpr std::uint32_t kStrX17 = 0xf81f0ff1u; // str x17, [sp, #-16]!
WORD_RE = re.compile(r'0x([0-9a-f]{8})u\s*[,;]\s*//\s*(.+)$')


def parse_words(path):
    words = []
    for line in open(path, encoding='utf-8').read().splitlines():
        m = WORD_RE.search(line)
        if m:
            words.append((int(m.group(1), 16), m.group(2).strip()))
    return words


def hex_to_dec(text):
    """#0x20 -> #32, #-0x10 -> #-16 (капстоун печатает шестнадцатерично)."""
    return re.sub(r'#(-?)0x([0-9a-f]+)',
                  lambda m: '#' + m.group(1) + str(int(m.group(2), 16)), text)


def clean(text):
    text = text.split('(')[0]              # отбросить пояснение в скобках
    text = hex_to_dec(text)
    text = text.replace(', #0]', ']')      # капстоун не печатает нулевое смещение
    return re.sub(r'\s+', ' ', text).strip().lower()


def decode(word, address):
    for ins in md.disasm(word.to_bytes(4, 'little'), address):
        return ins
    return None


def rendered(ins):
    """Текст инструкции, приведённый к виду комментария в исходнике."""
    op = ins.op_str
    # литера и переходы: капстоун печатает готовый адрес, в комментарии —
    # смещение от самой инструкции
    if ins.mnemonic in ('cbz', 'cbnz', 'b', 'bl', 'ldr') and ins.operands:
        last = ins.operands[-1]
        if last.type == capstone.arm64.ARM64_OP_IMM:
            op = re.sub(r'#(-?)0x[0-9a-f]+$', '#' + str(last.imm - ins.address), op)
    return clean('%s %s' % (ins.mnemonic, op))


def check(ok, msg, bad):
    print(('  ok   ' if ok else '  ПЛОХО ') + msg)
    return bad + (0 if ok else 1)


def main():
    words = parse_words(SRC)
    if len(words) < 12:
        print('ПЛОХО: в silent_patch.cpp не нашли кодов инструкций (%d)' % len(words))
        return 1
    print('кодов инструкций найдено: %d' % len(words))
    bad = 0

    for word, expected in words:
        ins = decode(word, 0x1000)
        if ins is None:
            bad = check(False, '%08x не разобрался (ожидалось %s)' % (word, expected), bad)
            continue
        got, want = rendered(ins), clean(expected)
        bad = check(got == want, '%08x -> %-28s | в исходнике: %s' % (word, got, want), bad)

    # Раскладка трамполина — в точности тот порядок, в котором его собирает
    # WritePatch() (movz/movk — по 4 инструкции на 64-битный адрес).
    layout = [
        ('movz x17, адрес блока', None),
        ('movk x17, lsl #16', None),
        ('movk x17, lsl #32', None),
        ('movk x17, lsl #48', None),
        ('ldrb w16, [x17, #0] — флаг', 0x39400230),
        ('cbz w16, #0x10 — мимо подмены, на пролог', 0x34000090),
        ('ldr s0, [x17, #8] — наша X', 0xbd400a20),
        ('ldr s1, [x17, #12] — наша Y', 0xbd400e21),
        ('ldr s2, [x17, #16] — наша Z', 0xbd401222),
        ('sub sp, sp, #0x40 (вытеснено)', 0xd10103ff),
        ('str x30, [sp, #0x10] (вытеснено)', 0xf9000bfe),
        ('stp x22, x21, [sp, #0x20] (вытеснено)', 0xa90257f6),
        ('stp x20, x19, [sp, #0x30] (вытеснено)', 0xa9034ff4),
        ('movz x16, сеттер+16', None),
        ('movk x16, lsl #16', None),
        ('movk x16, lsl #32', None),
        ('movk x16, lsl #48', None),
        ('br x16 — в тело сеттера', 0xd61f0200),
    ]
    print('\nраскладка трамполина:')
    addr_of = {}
    for i, (name, _word) in enumerate(layout):
        addr_of[i] = 4 * i
        print('  +%-4d %s' % (addr_of[i], name))

    print()
    cbz = decode(0x34000090, addr_of[5])
    bad = check(cbz.operands[1].imm == addr_of[9],
                'cbz ведёт ровно на вытесненный пролог (с %#x на %#x, нужно %#x)'
                % (addr_of[5], cbz.operands[1].imm, addr_of[9]), bad)

    for i, off in ((6, 8), (7, 12), (8, 16)):
        ins = decode(layout[i][1], 0x100)
        got = ins.operands[1].mem.disp
        bad = check(got == off, '%s читает смещение %d (нужно %d)'
                    % (layout[i][0], got, off), bad)

    ins = decode(0xd61f0200, 0x100)
    bad = check(ins.op_str == 'x16', 'br переходит по %s (нужно x16)' % ins.op_str, bad)
    ins = decode(0xd61f0220, 0x100)
    bad = check(ins.op_str == 'x17', 'br переходит по %s (нужно x17)' % ins.op_str, bad)
    ins = decode(0x58000051, 0x100)
    bad = check(ins.operands[1].imm == 0x108,
                'ldr x17 читает литеру по адресу %#x (нужно 0x108)' % ins.operands[1].imm, bad)

    # movz/movk: те же формулы, что в EmitMovz()/EmitMovk() в исходнике
    def movz(rd, imm):
        return 0xd2800000 | (imm << 5) | rd

    def movk(rd, imm, shift):
        return 0xf2800000 | ((shift // 16) << 21) | (imm << 5) | rd

    def load64(rd, value):
        return [movz(rd, value & 0xffff),
                movk(rd, (value >> 16) & 0xffff, 16),
                movk(rd, (value >> 32) & 0xffff, 32),
                movk(rd, (value >> 48) & 0xffff, 48)]

    for value, reg in ((0x7a5b8f5dbdc, 17), (0x71a2c34010, 16), (0xffffffffffffffff, 16)):
        code = b''.join(w.to_bytes(4, 'little') for w in load64(reg, value))
        text = '; '.join('%s %s' % (i.mnemonic, i.op_str)
                         for i in md.disasm(code, 0x1000))
        # капстоун печатает movz как mov, immediate шестнадцатерично, ноль как #0
        bad = check(clean(text) == clean('; '.join(['mov x%d, #0x%x' % (reg, value & 0xffff),
                                        'movk x%d, #0x%x, lsl #16' % (reg, (value >> 16) & 0xffff),
                                        'movk x%d, #0x%x, lsl #32' % (reg, (value >> 32) & 0xffff),
                                        'movk x%d, #0x%x, lsl #48' % (reg, (value >> 48) & 0xffff)])),
                    'загрузка %#x в x%d: %s' % (value, reg, text), bad)

    print()
    if bad:
        print('ПЛОХО: ошибок %d' % bad)
        return 1
    print('код патча сайлента: ОК')
    return 0


if __name__ == '__main__':
    sys.exit(main())

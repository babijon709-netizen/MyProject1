#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Прогон трамполина патча сайлента на эмуляторе ARM64 (unicorn).

encodings.py проверяет, что каждая инструкция закодирована верно. Здесь
проверяется главное — что трамполин в СБОРЕ ведёт себя правильно: выполняет
вытесненный пролог (кадр сеттера остаётся целым, x19/x20/x21/x22/x30 лежат
где сеттер их ищет), подменяет s0/s1/s2 нашей осью, когда патч включён, не
трогает их, когда выключен, и уходит ровно в тело сеттера, не испортив
стек и scratch-регистры.

Коды инструкций берутся прямо из jni/src/silent_patch.cpp — копии тут нет.

Требуется unicorn: pip install --target=.tools/unicorn unicorn
Без него проверка пропускается.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'jni', 'src', 'silent_patch.cpp')

try:
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_LITTLE_ENDIAN, UC_HOOK_CODE, UC_HOOK_MEM_WRITE_UNMAPPED
    from unicorn import arm64_const
except ImportError:
    print('unicorn не установлен — прогон трамполина пропущен')
    sys.exit(0)

TRAMP = 0x100000          # страница трамполина (её даёт mmap)
SETTER = 0x200000         # сеттер оси в условной игре
CTL = 0x150000            # блок управления (наша .bss)
STACK_TOP = 0x301000      # sp на входе в сеттер

# Константы вида:
#   constexpr std::uint32_t kStrX17 = 0xf81f0ff1u; // str x17, [sp, #-16]!
NAME_RE = re.compile(r'constexpr std::uint32_t\s+(\w+)\s*=\s*0x([0-9a-f]{8})u;')
# Массив пролога:
#   0xd10103ffu,  // sub sp, sp, #0x40
PROLOGUE_RE = re.compile(r'kPrologue\[4\] = \{(.*?)\};', re.S)


def load_source(path):
    text = open(path, encoding='utf-8').read()
    consts = {name: int(value, 16) for name, value in NAME_RE.findall(text)}
    prologue = [int(v, 16) for v in
                re.findall(r'0x([0-9a-f]{8})u', PROLOGUE_RE.search(text).group(1))]
    return consts, prologue


def movz(rd, imm):
    return 0xd2800000 | (imm << 5) | rd


def movk(rd, imm, shift):
    return 0xf2800000 | ((shift // 16) << 21) | (imm << 5) | rd


def mov64(rd, value):
    return [movz(rd, value & 0xffff),
            movk(rd, (value >> 16) & 0xffff, 16),
            movk(rd, (value >> 32) & 0xffff, 32),
            movk(rd, (value >> 48) & 0xffff, 48)]


def build(consts, prologue):
    """Тот же порядок, что в WritePatch()."""
    tramp = mov64(17, CTL)                             # адрес блока управления
    tramp += [consts['kLdrbActive'], consts['kCbzActive'],
              consts['kLdrS0'], consts['kLdrS1'], consts['kLdrS2']]
    tramp += prologue                                  # вытесненный пролог, один в один
    tramp += mov64(16, SETTER + 16)                    # в тело сеттера
    tramp += [consts['kBrX16']]

    patch = [consts['kLdrLitX17'], consts['kBrX17']]
    patch += list(struct.unpack('<II', struct.pack('<Q', TRAMP)))
    return tramp, patch


def words_to_bytes(words):
    return b''.join(w.to_bytes(4, 'little') for w in words)


def set_s(mu, index, value):
    """Положить float в sN (unicorn знает только 128-битные V-регистры,
    и пишет он их целым числом: младшие 32 бита — это и есть sN)."""
    bits = int.from_bytes(struct.pack('<f', value), 'little')
    mu.reg_write(getattr(arm64_const, 'UC_ARM64_REG_V%d' % index), bits)


def get_s(mu, index):
    raw = mu.reg_read(getattr(arm64_const, 'UC_ARM64_REG_V%d' % index))
    return struct.unpack('<f', (raw & 0xffffffff).to_bytes(4, 'little'))[0]


def set_x(mu, index, value):
    mu.reg_write(getattr(arm64_const, 'UC_ARM64_REG_X%d' % index), value)


def get_x(mu, index):
    return mu.reg_read(getattr(arm64_const, 'UC_ARM64_REG_X%d' % index))


def run(active, game_dir, our_dir, consts, prologue):
    """Прогнать patched setter -> трамполин -> тело сеттера.

    game_dir — ось, которую пишет игра; our_dir — наша ось в блоке управления.
    Тело сеттера подменяем перехватчиком: нас интересует состояние на входе в
    него (регистры и кадр), а не сама запись.
    """
    tramp, patch = build(consts, prologue)
    mu = Uc(UC_ARCH_ARM64, UC_MODE_LITTLE_ENDIAN)
    mu.mem_map(0x100000, 0x1000)          # трамполин
    mu.mem_map(0x150000, 0x1000)          # блок управления
    mu.mem_map(0x200000, 0x1000)          # условная страница сеттера
    mu.mem_map(0x300000, 0x4000)          # стек
    mu.mem_write(TRAMP, words_to_bytes(tramp))
    mu.mem_write(SETTER, words_to_bytes(patch))
    # дальше тела сеттера — 0x14000000: до него доходить не должны
    mu.mem_write(SETTER + 16, (0xd4200000).to_bytes(4, 'little'))  # brk #0
    mu.mem_write(CTL, bytes([1 if active else 0]) + b'\x00' * 7 +
                 struct.pack('<fff', *our_dir))

    set_s(mu, 0, game_dir[0]); set_s(mu, 1, game_dir[1]); set_s(mu, 2, game_dir[2])
    set_x(mu, 0, 0x4000)                  # this — объект оси
    set_x(mu, 1, 0x5555)                  # второй аргумент сеттера
    set_x(mu, 17, 0xaaaa)                 # в трамполине портится: проверим возврат
    set_x(mu, 19, 0x1919); set_x(mu, 20, 0x2020)
    set_x(mu, 21, 0x2121); set_x(mu, 22, 0x2222)
    set_x(mu, 30, 0x3030)                 # адрес возврата
    mu.reg_write(arm64_const.UC_ARM64_REG_SP, STACK_TOP)

    reached = []

    def hook_body(uc, address, size, user_data):
        reached.append(address)
        uc.emu_stop()

    mu.hook_add(UC_HOOK_CODE, hook_body, None, SETTER + 16, SETTER + 16)
    mu.emu_start(SETTER, SETTER + 12, count=64)

    sp = mu.reg_read(arm64_const.UC_ARM64_REG_SP)
    return {
        'reached': reached,
        's': (get_s(mu, 0), get_s(mu, 1), get_s(mu, 2)),
        'x0': get_x(mu, 0), 'x1': get_x(mu, 1), 'x16': get_x(mu, 16),
        'x17': get_x(mu, 17), 'x30': get_x(mu, 30),
        'sp': sp,
        'saved': struct.unpack('<QQQQQQ', mu.mem_read(sp + 0x10, 0x30)),
    }


def check(ok, msg, bad):
    print(('  ok   ' if ok else '  ПЛОХО ') + msg)
    return bad + (0 if ok else 1)


def main():
    consts, prologue = load_source(SRC)
    print('констант из silent_patch.cpp: %d, пролог: %d инструкции'
          % (len(consts), len(prologue)))
    bad = 0

    game_dir = (0.11, 0.22, 0.97)      # ось, которую пишет игра
    our_dir = (-0.5, 0.25, -0.83)      # наша ось

    print('\nпатч ВКЛЮЧЕН (игра пишет свою ось, должна уйти наша):')
    r = run(True, game_dir, our_dir, consts, prologue)
    bad = check(r['reached'] == [SETTER + 16],
                'трамполин пришёл ровно в тело сеттера (%s)'
                % ['%#x' % a for a in r['reached']], bad)
    for i in range(3):
        bad = check(abs(r['s'][i] - our_dir[i]) < 1e-6,
                    's%d = %.3f (наша ось %.3f), игра писала %.3f'
                    % (i, r['s'][i], our_dir[i], game_dir[i]), bad)

    print('\nкадр и регистры на входе в тело:')
    bad = check(r['sp'] == STACK_TOP - 0x40,
                'sp сдвинут ровно на кадр сеттера: %#x (нужно %#x)'
                % (r['sp'], STACK_TOP - 0x40), bad)
    bad = check(r['sp'] % 16 == 0, 'sp выровнен по 16 (%#x)' % r['sp'], bad)
    bad = check(r['x0'] == 0x4000 and r['x1'] == 0x5555 and r['x17'] != 0,
                'x16/x17 — scratch, трамполин вправе их менять', bad)
    bad = check(r['x30'] == 0x3030, 'x30 не тронут (%#x)' % r['x30'], bad)
    # пролог: [sp+0x10]=x30, [sp+0x18] не занят, [sp+0x20]=x22, [sp+0x28]=x21,
    # [sp+0x30]=x20, [sp+0x38]=x19 — именно так их забирает эпилог сеттера
    saved = r['saved']
    bad = check(saved[0] == 0x3030, 'в кадре x30 = %#x (нужно 0x3030)' % saved[0], bad)
    bad = check((saved[2], saved[3]) == (0x2222, 0x2121),
                'в кадре x22/x21 = %#x/%#x (нужно 0x2222/0x2121)' % (saved[2], saved[3]), bad)
    bad = check((saved[4], saved[5]) == (0x2020, 0x1919),
                'в кадре x20/x19 = %#x/%#x (нужно 0x2020/0x1919)' % (saved[4], saved[5]), bad)

    print('\nпатч ВЫКЛЮЧЕН (игра должна получить свою ось без изменений):')
    r2 = run(False, game_dir, our_dir, consts, prologue)
    bad = check(r2['reached'] == [SETTER + 16], 'дошли до тела сеттера', bad)
    for i in range(3):
        bad = check(abs(r2['s'][i] - game_dir[i]) < 1e-6,
                    's%d = %.3f — ось игры не тронута (наша %.3f)'
                    % (i, r2['s'][i], our_dir[i]), bad)
    bad = check(r2['sp'] == STACK_TOP - 0x40,
                'кадр сеттера в порядке и при выключенном патче', bad)

    print()
    if bad:
        print('ПЛОХО: ошибок %d' % bad)
        return 1
    print('трамполин патча сайлента: ОК')
    return 0


if __name__ == '__main__':
    sys.exit(main())

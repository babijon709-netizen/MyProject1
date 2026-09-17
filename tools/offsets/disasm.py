#!/usr/bin/env python3
# Дизассемблер дампов: RVA → инструкции и «кто это зовёт».
#
# Зачем. Смещения в game_offsets*.h нельзя брать «на глаз» из dump.cs: дамп
# показывает раскладку полей, но не то, кто и как их читает. Проверка по машинному
# коду — единственный способ убедиться, что поле действительно то (например, что
# пара углов прицела пишется именно так, как мы её пишем). Этот инструмент — тот
# же путь, которым проверялись 0x4C/0x28 у MouseLook и 0x2D4 у оружия.
#
# Подготовка дампов — tools/offsets/extract_dumps.sh (он же достаёт .so из .7z,
# включая ARM64 BCJ-фильтр, который не умеет py7zr).
#
# Использование:
#   python3 tools/offsets/disasm.py list <dump_dir> <Oxide.MouseLook> [макс]
#       методы класса с RVA (имена из dump.cs)
#   python3 tools/offsets/disasm.py dis <dump_dir> <rva> [кол-во инструкций]
#       дизассемблировать код по RVA (rva — как в dump.cs / script.json)
#   python3 tools/offsets/disasm.py xref <dump_dir> <rva> [--all]
#       кто вызывает эту функцию (BL/B), с именами методов вызывающих
#   python3 tools/offsets/disasm.py touch <dump_dir> <rva> <диапазон-N> <смещения>
#       в методе по RVA показать инструкции, трогающие [reg, #смещение]
#       для смещений из списка (через запятую, hex) — например 0x4c,0x88
#
# dump_dir — каталог, куда распакованы dump.cs / script.json / libil2cpp.so.
# capstone ставится сам (pip), если его нет.
import os
import re
import sys
import subprocess


def ensure_capstone():
    try:
        import capstone  # noqa: F401
    except ImportError:
        subprocess.run([sys.executable, '-m', 'pip', 'install', '-q', '--break-system-packages',
                        'capstone'], check=True)
    import capstone
    return capstone


def sections(so_path):
    """Исполняемые секции .so: имя, адрес, смещение в файле, размер.

    Код игры лежит не в .text, а в секции `il2cpp` — поэтому секции читаются из
    заголовков ELF, а не берутся по имени.
    """
    with open(so_path, 'rb') as f:
        data = f.read(0x40)
        if data[:4] != b'\x7fELF':
            raise SystemExit('не ELF: %s' % so_path)
        e_shoff = int.from_bytes(data[0x28:0x30], 'little')
        e_shentsize = int.from_bytes(data[0x3A:0x3C], 'little')
        e_shnum = int.from_bytes(data[0x3C:0x3E], 'little')
        e_shstrndx = int.from_bytes(data[0x3E:0x40], 'little')
        if not e_shoff or not e_shnum:
            return []
        f.seek(e_shoff + e_shstrndx * e_shentsize)
        strtab_hdr = f.read(e_shentsize)
        strtab_off = int.from_bytes(strtab_hdr[0x18:0x20], 'little')
        out = []
        for index in range(e_shnum):
            f.seek(e_shoff + index * e_shentsize)
            sh = f.read(e_shentsize)
            name_off = int.from_bytes(sh[0x00:0x04], 'little')
            sh_type = int.from_bytes(sh[0x04:0x08], 'little')
            flags = int.from_bytes(sh[0x08:0x10], 'little')
            addr = int.from_bytes(sh[0x10:0x18], 'little')
            offset = int.from_bytes(sh[0x18:0x20], 'little')
            size = int.from_bytes(sh[0x20:0x28], 'little')
            if sh_type != 1 or size == 0:          # только PROGBITS
                continue
            f.seek(strtab_off + name_off)
            name = b''
            while True:
                ch = f.read(1)
                if ch in (b'\x00', b''):
                    break
                name += ch
            if flags & 0x4:                        # SHF_EXECINSTR
                out.append((name.decode('utf-8', 'replace'), addr, offset, size))
        return out


def read_code(so_path, rva, size):
    with open(so_path, 'rb') as f:
        for _name, addr, offset, sec_size in sections(so_path):
            if addr <= rva < addr + sec_size:
                f.seek(offset + (rva - addr))
                return f.read(min(size, addr + sec_size - rva))
    return None


def dump_dir():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    return sys.argv[2]


def so_of(dump):
    path = os.path.join(dump, 'libil2cpp.so')
    if not os.path.exists(path):
        raise SystemExit('нет %s — распакуй дампы: tools/offsets/extract_dumps.sh %s' % (path, dump))
    return path


def script_methods(dump, prefix=None):
    """Список (RVA, имя) из script.json. Address — уже RVA.

    Файл читается ПОТОКОВО, строка за строкой: он красивый (по полю на строку), а
    целиком занимает сотни мегабайт в памяти — на релизном дампе json.load()
    вместе с дизассемблированием упирался в предел памяти песочницы, и процесс
    убивало без единой строки вывода. С префиксом собираются только нужные
    методы (для `list`), без него — все (для `xref`).
    """
    path = os.path.join(dump, 'script.json')
    if not os.path.exists(path):
        return []
    out = []
    address = None
    find_address = re.compile(r'"Address":\s*(-?\d+)')
    find_name = re.compile(r'"Name":\s*"((?:[^"\\]|\\.)*)"')
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if address is None:
                m = find_address.search(line)
                if m:
                    address = int(m.group(1))
                continue
            m = find_name.search(line)
            if m:
                name = m.group(1)
                if prefix is None or name.startswith(prefix):
                    out.append((address, name))
            address = None
    out.sort()
    return out


def command_list():
    dump = dump_dir()
    prefix = sys.argv[3]
    limit = int(sys.argv[4]) if len(sys.argv) > 4 else 200
    found = script_methods(dump, prefix)
    for rva, name in found[:limit]:
        print('%#010x  %s' % (rva, name))
    print('-- всего %d' % len(found))


def command_dis():
    dump = dump_dir()
    rva = int(sys.argv[3], 16) if sys.argv[3].startswith('0x') else int(sys.argv[3])
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 40
    capstone = ensure_capstone()
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    data = read_code(so_of(dump), rva, count * 4 + 64)
    if data is None:
        raise SystemExit('RVA %#x не в исполняемой секции' % rva)
    names = dict(script_methods(dump))
    if rva in names:
        print('== %s' % names[rva])
    for index, insn in enumerate(md.disasm(data, rva)):
        if index >= count:
            break
        target = ''
        for op in insn.operands:
            if op.type == capstone.arm64.ARM64_OP_IMM and insn.mnemonic in ('bl', 'b') and \
                    op.imm in names:
                target = '   ; %s' % names[op.imm]
        print('%08x  %-8s %s%s' % (insn.address, insn.mnemonic, insn.op_str, target))


def _find_calls(data, base, target):
    """Адреса вызовов target в куске кода по адресу base (без дизассемблера).

    ARM64: BL и B — это одна инструкция с кодом операции в старших шести битах
    (100101 и 000101), а адрес перехода — знаковое смещение в словах. Проверять
    так в разы быстрее, чем гонять capstone по сотне мегабайт кода, и памяти
    почти не нужно. Именно так находится «кто зовёт» — в том числе хвостовые
    переходы (игра нередко заканчивает метод переходом на общий код).
    """
    hits = []
    try:
        import numpy as np
        words = np.frombuffer(data, dtype='<u4')
        ops = words & np.uint32(0xFC000000)
        idx = np.nonzero((ops == 0x94000000) | (ops == 0x14000000))[0]
        for i in idx:
            w = int(words[i])
            imm = w & 0x03FFFFFF
            if imm & 0x02000000:
                imm -= 0x04000000
            if base + int(i) * 4 + imm * 4 == target:
                hits.append(base + int(i) * 4)
        return hits
    except ImportError:
        pass
    import struct
    for off in range(0, len(data) - 3, 4):
        word = struct.unpack_from('<I', data, off)[0]
        if (word & 0xFC000000) not in (0x94000000, 0x14000000):
            continue
        imm = word & 0x03FFFFFF
        if imm & 0x02000000:
            imm -= 0x04000000
        if base + off + imm * 4 == target:
            hits.append(base + off)
    return hits


def command_xref():
    dump = dump_dir()
    target = int(sys.argv[3], 16) if sys.argv[3].startswith('0x') else int(sys.argv[3])
    so_path = so_of(dump)
    found = []
    for name, addr, offset, size in sections(so_path):
        with open(so_path, 'rb') as f:
            f.seek(offset)
            data = f.read(size)
        for hit in _find_calls(data, addr, target):
            found.append((name, hit))
    if found:
        names = script_methods(dump)
        for section_name, hit in sorted(found, key=lambda item: item[1]):
            caller = 0
            caller_name = '?'
            for rva, name in names:          # список отсортирован: идём до места вызова
                if rva > hit:
                    break
                caller, caller_name = rva, name
            print('%-10s %08x  в %s' % (section_name, hit, caller_name))
    print('-- вызовов: %d' % len(found))


def command_touch():
    dump = dump_dir()
    rva = int(sys.argv[3], 16) if sys.argv[3].startswith('0x') else int(sys.argv[3])
    span = int(sys.argv[4], 16) if sys.argv[4].startswith('0x') else int(sys.argv[4])
    wanted = {int(x.strip(), 16) for x in sys.argv[5].split(',')}
    capstone = ensure_capstone()
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    data = read_code(so_of(dump), rva, span)
    if data is None:
        raise SystemExit('RVA %#x не в исполняемой секции' % rva)
    for insn in md.disasm(data, rva):
        for op in insn.operands:
            if op.type == capstone.arm64.ARM64_OP_MEM and op.mem.disp in wanted:
                print('%08x  %-8s %s' % (insn.address, insn.mnemonic, insn.op_str))
                break


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    command = sys.argv[1]
    {'list': command_list, 'dis': command_dis, 'xref': command_xref,
     'touch': command_touch}.get(command, lambda: (_ for _ in ()).throw(SystemExit(__doc__)))()
    if not os.path.exists('/usr/local/lib/python3.11/dist-packages/capstone') and False:
        ensure_capstone()


if __name__ == '__main__':
    main()

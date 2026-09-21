#!/usr/bin/env python3
"""Verify fast-loot instruction sites against the extracted game images.
Usage: python3 tools/fast_loot/check_native.py release/libil2cpp.so beta/libil2cpp.so
Does not modify images. No disassembler or third-party dependencies required.
"""
import pathlib
import re
import struct
import sys

header = pathlib.Path(__file__).resolve().parents[2] / 'jni/src/fast_loot_memory.h'
text = header.read_text()
rows = re.findall(r'\{\{(0x[0-9a-f, x]+)\},\s*(0x[0-9a-f]+),\s*\{(0x[0-9a-f, x]+)\}\}', text)
if len(sys.argv) != 3 or len(rows) != 2:
    raise SystemExit(__doc__)
patch = int(re.search(r'kFastInstruction = (0x[0-9a-f]+)', text)[1], 16)
assert patch & ~0x3ff == 0x1e270000, 'not FMOV S, W'
assert (patch >> 5) & 31 == 31, 'source must be WZR'
assert patch & 31 == 0, 'destination must be S0'
# WZR is all-zero bits, so the float argument and rounded timer are exactly zero.
fast = struct.unpack('<f', struct.pack('<I', 0))[0]
fast_ms = round(fast * 1000)
assert fast_ms == 0
for filename, (id_text, site_text, guard_text) in zip(sys.argv[1:], rows):
    expected_id = bytes(int(v.strip(), 16) for v in id_text.split(','))
    site = int(site_text, 16)
    guard = tuple(int(v.strip(), 16) for v in guard_text.split(','))
    data = pathlib.Path(filename).read_bytes()
    assert data[:6] == b'\x7fELF\x02\x01'
    assert struct.unpack_from('<H', data, 0x12)[0] == 183, 'expected AArch64'
    phoff = struct.unpack_from('<Q', data, 0x20)[0]
    phsize, phnum = struct.unpack_from('<HH', data, 0x36)
    segments = [struct.unpack_from('<IIQQQQQQ', data, phoff+i*phsize) for i in range(phnum)]

    def read(va, size):
        for kind, flags, off, addr, _, filesz, memsz, align in segments:
            if kind == 1 and addr <= va and va+size <= addr+filesz:
                return data[off+va-addr:off+va-addr+size]
        raise AssertionError(f'RVA {va:#x} not file-backed')

    assert struct.unpack('<III', read(0x308, 12)) == (4, 20, 3)
    assert read(0x314, 4) == b'GNU\0'
    assert read(0x318, 20) == expected_id, 'build ID mismatch'
    assert site % 4 == 0
    assert any(k == 1 and fl & 1 and va <= site-12 and site+20 <= va+fs
               for k, fl, off, va, _, fs, ms, al in segments), 'guard outside executable segment'
    assert struct.unpack('<8I', read(site-12, 32)) == guard, 'code fingerprint mismatch'
    adrp, load = guard[0], guard[3]
    assert adrp & 0x9f00001f == 0x90000008, 'expected ADRP X8'
    assert load & 0xffc003ff == 0xbd400100, 'expected LDR S0, [X8, imm]'
    imm = ((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3)
    if imm & (1 << 20):
        imm -= 1 << 21
    constant = ((site-12) & ~0xfff) + (imm << 12) + ((load >> 10) & 0xfff)*4
    original = struct.unpack('<f', read(constant, 4))[0]
    assert abs(original - 0.35) < 1e-6, 'original is not the 0.35s collect wait'
    assert guard[7] & 0xfc000000 == 0x94000000, 'delay call changed'
    delta = guard[7] & 0x3ffffff
    if delta & (1 << 25):
        delta -= 1 << 26
    wait = site + 16 + delta*4
    # Verify the conversion used above: scale by 1000, add 0.5 / floor for
    # nonnegative non-halfway inputs, then convert the rounded value to int32.
    for offset, opcode in ((0x14, 0x1e204008), (0x48, 0x52a88f49),
                           (0x50, 0x1e270129), (0x74, 0x1e290909),
                           (0xc4, 0x1e6c1001), (0xf8, 0x1e612900),
                           (0xfc, 0x1e654008), (0x114, 0x1e780109)):
        assert struct.unpack('<I', read(wait+offset, 4))[0] == opcode, 'WaitForSeconds conversion changed'
    print(f'{filename}: build ID, RX guard, site {site:#x}, {original:.3f}s -> {fast_ms}ms timer: OK')

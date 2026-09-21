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
assert patch & 0xfc000000 == 0x14000000, 'must be B, not a zero timer or BL'
resume_offset = int(re.search(r'kResumeOffset = (0x[0-9a-f]+)', text)[1], 16)
resume_text = re.search(r'kResumeGuard = \{([^}]+)\}', text)[1]
resume_guard = tuple(int(v.strip(), 16) for v in resume_text.split(','))
assert (patch & 0x3ffffff) * 4 == resume_offset
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
    resume = site + resume_offset
    tail = struct.unpack('<4I', read(resume, 16))
    assert tail == resume_guard, 'next-slot tail changed'
    assert tail[:3] == (0xb9403a68, 0x51000508, 0xb9003a68), 'must decrement existing sourceIndex'
    tbz = tail[3]
    assert tbz & 0xfff8001f == 0x36f80008, 'must loop while W8 bit 31 is zero'
    delta = (tbz >> 5) & 0x3fff
    if delta & (1 << 13):
        delta -= 1 << 14
    loop = resume + 12 + delta*4
    assert loop == site - 0x1c0, 'branch no longer returns to the per-slot validation path'
    # Validate existing container/state/slot checks at that destination, and
    # the transfer call BEFORE our jump. No jump over a transfer or its checks.
    for offset, opcode in ((0, 0xf94002c0), (4, 0xf9400e74),
                           (0x20, 0x36001920), (0x2c, 0xf9404108),
                           (0x48, 0x370017e0), (0x90, 0x5400152d)):
        assert struct.unpack('<I', read(loop+offset, 4))[0] == opcode, 'source checks changed'
    assert struct.unpack('<I', read(site-0x24, 4))[0] & 0xfc000000 == 0x94000000, 'transfer call missing'
    assert site < site+16 < resume, 'await call must be skipped by the forward branch'
    # Interpret the guarded decrement/test tail for empty and multi-slot sources.
    # This tests the native control-flow contract, not server acceptance of RPCs.
    for count in (0, 1, 12, 30, 60, 96):
        index = count-1
        visited = []
        while index >= 0:
            visited.append(index)
            index -= 1
            assert len(visited) <= count, 'unbounded collection loop'
        assert visited == list(range(count-1, -1, -1))
    print(f'{filename}: exact image, {site:#x} -> {resume:#x}, guarded next-slot loop {loop:#x}, no await: OK')

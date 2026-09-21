#!/usr/bin/env python3
"""Verify FPS layout fingerprints against extracted release/beta libunity.so.
Usage: python3 tools/viewmodel/check_unity_fps.py release.so beta.so
No disassembler or third-party packages required. Not run by Android CI:
unit tests exercise runtime guards; this check needs the native game images.
"""
import pathlib
import re
import struct
import sys

header = pathlib.Path(__file__).resolve().parents[2] / 'jni/src/fps_memory.h'
layouts = re.findall(r'\{(0x[0-9a-f]+)ULL, (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)\}', header.read_text())
if len(sys.argv) != 3 or len(layouts) != 2:
    raise SystemExit(__doc__)
for filename, row in zip(sys.argv[1:], layouts):
    build_id, getter, target, load, pacing_update, pacing_type = map(lambda v: int(v, 16), row)
    data = pathlib.Path(filename).read_bytes()
    assert data[:6] == b'\x7fELF\x02\x01', 'expected little-endian ELF64'
    assert struct.unpack_from('<H', data, 0x12)[0] == 183, 'expected AArch64'
    phoff = struct.unpack_from('<Q', data, 0x20)[0]
    phsize, phnum = struct.unpack_from('<HH', data, 0x36)
    segments = [struct.unpack_from('<IIQQQQQQ', data, phoff+i*phsize) for i in range(phnum)]

    def read(va, size):
        for kind, flags, off, addr, _, filesz, memsz, align in segments:
            if kind == 1 and addr <= va and va+size <= addr+filesz:
                return data[off+va-addr:off+va-addr+size]
        raise AssertionError(f'RVA {va:#x} is not file-backed')

    assert struct.unpack('<Q', read(0x318, 8))[0] == build_id, 'build ID mismatch'
    code = struct.unpack('<6I', read(getter, 24))
    assert code[:3] == (0xf00083e8, load, 0xd65f03c0), 'getter fingerprint mismatch'
    # Adjacent setter uses the same ADRP and corresponding STR W0, then calls
    # the Android pacing update. We do not execute or patch that native code.
    assert code[3:5] == (code[0], load ^ 0x00400000), 'setter targets another field'
    assert code[5] & 0xfc000000 == 0x14000000, 'setter tailcall changed'
    assert read(pacing_update, 4) == struct.pack('<I', 0xf81e0ffe), 'pacing update fingerprint'
    assert any(k == 1 and fl & 2 and va <= pacing_type < va+ms
               for k, fl, off, va, _, fs, ms, al in segments), 'pacing type not writable'
    imm = ((code[0] >> 5) & 0x7ffff) << 2 | ((code[0] >> 29) & 3)
    if imm & (1 << 20):
        imm -= 1 << 21
    actual_target = (getter & ~0xfff) + (imm << 12) + ((load >> 10) & 0xfff)*4
    assert actual_target == target, 'incorrect ADRP/LDR target'
    assert any(k == 1 and fl & 2 and va <= target < va+ms
               for k, fl, off, va, _, fs, ms, al in segments), 'target not writable'
    print(f'{filename}: build ID, getter/setter, writable target {target:#x}, pacing {pacing_type:#x}: OK')

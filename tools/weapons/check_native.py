#!/usr/bin/env python3
"""Verify reviewed weapon consumers against release/beta libil2cpp images.

python3 tools/weapons/check_native.py release.so beta.so
python3 tools/weapons/check_native.py --emulate release.so beta.so

The optional execution test requires `pip install unicorn`. It executes the
real ARM64 arithmetic and recoil-store blocks, not a full game/network client.
The input files are never modified. Patch definitions come from the C++ header.
"""
import argparse
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / 'jni/src/weapon_tuning_memory.h').read_text()
IDS = re.findall(r'\{\{(0x[0-9a-f,x]+)\}, \{\{', HEADER)
SITES = re.findall(r'\{(0x[0-9a-f]+), \{(0x[0-9a-f,x]+)\}\}', HEADER)
RECOIL = int(re.search(r'kNoRecoil = (0x[0-9a-f]+)', HEADER)[1], 16)
SPREAD = int(re.search(r'kNoSpread = (0x[0-9a-f]+)', HEADER)[1], 16)
assert len(IDS) == 2 and len(SITES) == 6
assert RECOIL == 0xf900013f, 'must store both zero axes, not suppress firing'
assert SPREAD == 0x1e2703e0, 'must return exact +0.0 in S0'

# Independent consumer evidence (not part of the runtime patch table).
EVIDENCE = [
    dict(fixed=0x660be14, recoil_offset=0x2d4,
         dynamic=0x7342560, base=0x73425d8, mobility=None, constant=0x21351e0,
         dynamic_calls=[0x6602214,0x660467c,0x6605848,0x66073f0],
         base_calls=[0x660238c,0x66046a4,0x660599c,0x6607568]),
    dict(fixed=0x6565730, recoil_offset=0x2dc,
         dynamic=0x72dadcc, base=0x72db02c, mobility=0x72bc110, constant=0x2131acc,
         dynamic_calls=[0x656489c,0x65656ac,0x65693d4],
         base_calls=[0x65649f0,0x65656d4,0x6569544]),
]


class Image:
    def __init__(self, filename):
        self.data = Path(filename).read_bytes()
        assert self.data[:6] == b'\x7fELF\x02\x01'
        assert struct.unpack_from('<H', self.data, 0x12)[0] == 183
        phoff = struct.unpack_from('<Q', self.data, 0x20)[0]
        phsize, phnum = struct.unpack_from('<HH', self.data, 0x36)
        self.segments = [struct.unpack_from('<IIQQQQQQ', self.data, phoff+i*phsize)
                         for i in range(phnum)]

    def read(self, va, size):
        for kind, flags, off, addr, _, filesz, memsz, align in self.segments:
            if kind == 1 and addr <= va and va+size <= addr+filesz:
                return self.data[off+va-addr:off+va-addr+size]
        raise AssertionError(f'RVA {va:#x} not file-backed')

    def word(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]

    def rx(self, va, size):
        return any(k == 1 and fl & 1 and addr <= va and va+size <= addr+fs
                   for k, fl, off, addr, _, fs, ms, al in self.segments)


def verify(image, build_id, sites, evidence):
    assert image.read(0x308, 16) == struct.pack('<4I', 4,20,3,0x00554e47)
    assert image.read(0x318, 20) == build_id
    for rva, guard in sites:
        assert rva % 4 == 0 and image.rx(rva-12, 32)
        assert struct.unpack('<8I', image.read(rva-12, 32)) == guard
    recoil, dynamic, base = [r for r, _ in sites]
    assert evidence['fixed'] < recoil < evidence['fixed']+0x800
    # X9 points at the FPHitscan Vector2 accumulator; not ammo or player health.
    assert image.word(recoil-0x34) == (0x91000269 | evidence['recoil_offset'] << 10)
    assert image.word(recoil) == 0xfd000120  # STR D0,[X9]
    assert image.word(dynamic) == 0x1e214c40  # final clamped dynamic spread
    assert image.word(base) == 0x1e210800     # final ADS-scaled base spread
    for key, target in [('dynamic_calls', evidence['dynamic']), ('base_calls', evidence['base'])]:
        for call in evidence[key]:
            word = image.word(call)
            assert word & 0xfc000000 == 0x94000000
            delta = word & 0x3ffffff
            if delta & (1 << 25):
                delta -= 1 << 26
            assert call + delta*4 == target, 'shot/crosshair consumer no longer calls reviewed calculator'


def emulate(image, sites, evidence):
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_W1, UC_ARM64_REG_X19,
                                    UC_ARM64_REG_X20, UC_ARM64_REG_X30, UC_ARM64_REG_SP,
                                    UC_ARM64_REG_PC, UC_ARM64_REG_S0, UC_ARM64_REG_S1,
                                    UC_ARM64_REG_S8, UC_ARM64_REG_S9)
    data, stack, stop = 0x10000000, 0x10002000, 0x10004000

    def machine():
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        pages = set()
        def map_image(rva):
            page = rva & ~4095
            if page not in pages:
                uc.mem_map(page, 4096)
                uc.mem_write(page, image.read(page, 4096))
                pages.add(page)
        for rva, _ in sites:
            map_image(rva-0x40); map_image(rva+0x20)
        map_image(evidence['dynamic']); map_image(evidence['base'])
        map_image(evidence['constant'])
        if evidence['mobility']:
            map_image(evidence['mobility'])
        uc.mem_map(data, 4096)
        uc.mem_map(stack, 4096)
        uc.mem_map(stop, 4096)
        return uc

    def bits(value):
        return struct.unpack('<I', struct.pack('<f', value))[0]

    def call(uc, start, end=stop):
        uc.reg_write(UC_ARM64_REG_SP, stack+2048)
        uc.reg_write(UC_ARM64_REG_X30, stop)
        uc.emu_start(start, end, count=500)
        assert uc.reg_read(UC_ARM64_REG_PC) == end, 'unexpected branch or loop'
        assert uc.reg_read(UC_ARM64_REG_SP) == stack+2048, 'stack was not restored'

    # Execute original -> patched -> restored on fresh instruction caches.
    # Start after WeaponRecoil returns its two scalar axes, end after the store.
    for patched in (False, True, False):
        for shot_count in (0, 1, 30):
            for aim_factor in (1.0, 0.4):
                uc = machine()
                rva = sites[0][0]
                if patched:
                    uc.mem_write(rva, struct.pack('<I', RECOIL))
                uc.reg_write(UC_ARM64_REG_X19, data)
                uc.reg_write(UC_ARM64_REG_S0, bits(1.25))
                uc.reg_write(UC_ARM64_REG_S1, bits(-0.75))
                uc.reg_write(UC_ARM64_REG_S8, bits(0.85))
                uc.reg_write(UC_ARM64_REG_S9, bits(aim_factor))
                uc.mem_write(data+0x230, struct.pack('<I', shot_count))
                uc.mem_write(data+evidence['recoil_offset'], struct.pack('<2f', 0.3, -0.4))
                call(uc, rva-0x3c, rva+4)
                result = bytes(uc.mem_read(data+evidence['recoil_offset'], 8))
                assert (result == bytes(8)) == patched
                assert bytes(uc.mem_read(data+0x230, 4)) == struct.pack('<I', shot_count+1)
                assert bytes(uc.mem_read(data+0x22c, 1)) == b'\x01'

    # Full real dispersion functions, including beta's real mobility helper.
    # Exercise hipfire/ADS, movement, idle/burst values, lower/upper clamps.
    for patched in (False, True, False):
        for aimed in (0, 1):
            for fire in (0.0, 1.0, 30.0):
                for movement in (0.0, 0.7, 4.0):
                    for index, entry in [(1,evidence['dynamic']), (2,evidence['base'])]:
                        uc = machine()
                        if patched:
                            uc.mem_write(sites[index][0], struct.pack('<I', SPREAD))
                        # calculator -> dispersion config, weapon config, cached base value
                        uc.mem_write(data+0x10, struct.pack('<QQf', data+0x100, data+0x200, 1.8))
                        uc.mem_write(data+0x120, struct.pack('<2f', 0.2, 3.0))
                        uc.mem_write(data+0x130, struct.pack('<4f', 0.6, 0.4, 0.3, 0.5))
                        uc.mem_write(data+0x240, struct.pack('<f', 2.0))
                        uc.reg_write(UC_ARM64_REG_X0, data)
                        uc.reg_write(UC_ARM64_REG_W1, aimed)
                        uc.reg_write(UC_ARM64_REG_X19, 0x1234)
                        uc.reg_write(UC_ARM64_REG_X20, 0x5678)
                        uc.reg_write(UC_ARM64_REG_S0, bits(fire))
                        uc.reg_write(UC_ARM64_REG_S1, bits(movement))
                        call(uc, entry)
                        output_bits = uc.reg_read(UC_ARM64_REG_S0)
                        assert (output_bits == 0) == patched
                        result = struct.unpack('<f', struct.pack('<I', output_bits))[0]
                        if not patched:
                            if index == 1:
                                assert 0.1999 <= result <= 3.0001
                            else:
                                assert abs(result - (0.72 if aimed else 1.8)) < 1e-6
                        assert uc.reg_read(UC_ARM64_REG_X19) == 0x1234
                        assert uc.reg_read(UC_ARM64_REG_X20) == 0x5678
    print('  ARM64 execution: recoil axes, shot counter, hipfire/ADS, movement/burst, clamps, stack, restore: OK')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emulate', action='store_true')
    parser.add_argument('release')
    parser.add_argument('beta')
    args = parser.parse_args()
    for i, filename in enumerate([args.release, args.beta]):
        image = Image(filename)
        build_id = bytes(int(v,16) for v in IDS[i].split(','))
        sites = [(int(rva,16), tuple(int(v,16) for v in guard.split(',')))
                 for rva,guard in SITES[i*3:i*3+3]]
        verify(image, build_id, sites, EVIDENCE[i])
        print(f'{filename}: exact GNU ID, three RX guards, recoil/shot/crosshair consumers: OK')
        if args.emulate:
            emulate(image, sites, EVIDENCE[i])


if __name__ == '__main__':
    main()

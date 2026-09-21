#!/usr/bin/env python3
"""Re-derive the instant-pickup table of jni/src/loot_memory.h for a game image.

    find_table.py libil2cpp.so            # print the row for a new build
    find_table.py libil2cpp.so --check    # ...and assert it equals the shipped one

Requires capstone. Nothing here is a guess and nothing is typed in by hand:

  * the three stock transfer coroutines are the only code in the image that
    awaits exactly 0.35 s through PlayerLoopTiming.Update with ignoreTimeScale;
  * each is recognised by the shape of its ``AsyncUniTask`1<...>::Run`` — it
    takes the delegate's m_target at +0x20, the state machine at runner+0x20 and
    tail-branches into MoveNext, which is exactly what the identity chain in
    loot_memory.h matches on;
  * the delay word printed here is the ORIGINAL instruction at the site, so the
    controller can keep refusing a process that carries the retired patch.

PlayerLoopHelper's TypeInfo slot is derived separately (the class name is not
obfuscated, it belongs to UniTask):

    python3 tools/offsets/typeinfo_rva.py --so libil2cpp.so --script script.json \\
        Cysharp.Threading.Tasks.PlayerLoopHelper

After a game update: run this on both images, replace the rows in
jni/src/loot_memory.h, confirm which flow is which against the fresh dump.cs
(the order decides the reported Kind), then re-run `sh tools/loot/run.sh`.
See FAST_LOOT.md.
"""
import collections
import mmap
import struct
import sys
from pathlib import Path

try:
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
except ImportError as exc:                                  # pragma: no cover
    raise SystemExit('capstone is required: pip install capstone (%s)' % exc)

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_native import HEADER, parse_table                # noqa: E402

# AsyncUniTask`1<T>::Run, every instantiation: m_target -> &stateMachine -> vtable.
RUN_PROLOGUE = (0xf9401028, 0x91008000, 0xf9406108, 0xf9404101)
DELAY_SECONDS = 0.35
WINDOW = 0x900          # how far into MoveNext the await can sit
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
md.detail = True


class Image:
    def __init__(self, path):
        with open(path, 'rb') as handle:
            self.data = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
        data = self.data
        assert data[:6] == b'\x7fELF\x02\x01', 'not an ARM64 ELF: %s' % path
        self.segments = []
        phoff = struct.unpack_from('<Q', data, 0x20)[0]
        entsize, count = struct.unpack_from('<HH', data, 0x36)
        for i in range(count):
            kind, flags, off, va, _pa, filesz, _memsz, _align = \
                struct.unpack_from('<II6Q', data, phoff + i * entsize)
            if kind == 1:
                self.segments.append((va, off, filesz, flags))

    def build_id(self):
        return self.data[0x318:0x318 + 20]

    def to_va(self, off):
        for va, seg, filesz, _flags in self.segments:
            if seg <= off < seg + filesz:
                return va + (off - seg)
        return None

    def to_off(self, va):
        for seg_va, off, filesz, _flags in self.segments:
            if seg_va <= va < seg_va + filesz:
                return off + (va - seg_va)
        return None

    def at(self, va, size):
        off = self.to_off(va)
        return b'' if off is None else self.data[off:off + size]

    def word(self, va):
        raw = self.at(va, 4)
        return None if len(raw) != 4 else struct.unpack('<I', raw)[0]

    def disasm(self, va, size):
        return md.disasm(self.at(va, size), va)

    def executable_offsets(self, pattern):
        """File offsets of every occurrence of `pattern` in an R+X segment."""
        out, cursor = [], 0
        while True:
            hit = self.data.find(pattern, cursor)
            if hit < 0:
                return out
            cursor = hit + 4
            for _va, off, filesz, flags in self.segments:
                if flags & 1 and off <= hit < off + filesz:
                    out.append(hit)
                    break


def branch_target(image, va, word):
    if (word & 0xFC000000) != 0x14000000:
        return None
    imm = word & 0x3FFFFFF
    if imm & 0x2000000:
        imm -= 0x4000000
    return va + imm * 4


def find_delay(image, move_next):
    """The 0.35 s / ignoreTimeScale / Update await inside this MoveNext.

    Register allocation is not assumed: the literal has to land in a
    single-precision register from the register the preceding `adrp` produced,
    and the two call arguments set just before it have to be ignoreTimeScale = 1
    and PlayerLoopTiming.Update = 8.
    """
    bits = struct.unpack('<I', struct.pack('<f', DELAY_SECONDS))[0]
    page, page_reg, recent = None, None, collections.deque(maxlen=6)
    for ins in image.disasm(move_next, WINDOW):
        if ins.mnemonic == 'adrp':
            page, page_reg = ins.operands[1].imm, md.reg_name(ins.operands[0].reg)
        elif ins.mnemonic == 'mov':
            recent.append(ins.op_str)
        elif ins.mnemonic == 'ldr' and page is not None and len(ins.operands) == 2:
            dest = md.reg_name(ins.operands[0].reg)
            mem = ins.operands[1].mem
            if not dest or dest[0] != 's' or not mem.base:
                continue
            if md.reg_name(mem.base) != page_reg:
                continue
            raw = image.at(page + mem.disp, 4)
            if len(raw) != 4 or struct.unpack('<I', raw)[0] != bits:
                continue
            if 'w0, #1' not in recent or 'w1, #8' not in recent:
                continue          # not ignoreTimeScale + PlayerLoopTiming.Update
            helper = None
            for after in image.disasm(ins.address + 4, 0x20):
                if after.mnemonic == 'bl':
                    helper = after.operands[0].imm
                    break
            return ins.address, image.word(ins.address), helper
    return None


def derive(path):
    image = Image(path)
    pattern = struct.pack('<%dI' % len(RUN_PROLOGUE), *RUN_PROLOGUE)
    flows, helpers, prologues = [], set(), 0
    for offset in image.executable_offsets(pattern):
        run = image.to_va(offset)
        branch = image.word(run + 16)
        move_next = branch_target(image, run + 16, branch) if branch else None
        if move_next is None:
            continue
        prologues += 1
        found = find_delay(image, move_next)
        if not found:
            continue
        site, word, helper = found
        flows.append((run, move_next, site, word))
        helpers.add(helper)
    assert len(helpers) <= 1, 'the flows await through %s' % sorted(hex(h) for h in helpers)
    flows.sort()
    return {'build_id': image.build_id(), 'flows': flows, 'helper': helpers.pop(),
            'prologues': prologues}


def report(path, found):
    print('%s' % path)
    print('  build-id %s   (%d AsyncUniTask`1 Run prologues scanned)'
          % (found['build_id'].hex(), found['prologues']))
    print('  awaited through UniTask.WaitForSeconds %#x' % found['helper'])
    for run, move_next, site, word in found['flows']:
        print('    {0x%X, 0x%X, 0x%X, 0x%08Xu},   // Run, MoveNext, delay site, original word'
              % (run, move_next, site, word))
    print('  PlayerLoopHelper TypeInfo: python3 tools/offsets/typeinfo_rva.py --so %s '
          '--script script.json Cysharp.Threading.Tasks.PlayerLoopHelper' % path)


def check(path, found, layouts):
    row = next((l for l in layouts if l['build_id'] == found['build_id']), None)
    assert row is not None, \
        'build-id %s is not in jni/src/loot_memory.h — add the row above' % found['build_id'].hex()
    shipped = {tuple(f) for f in row['flows']}
    derived = set(found['flows'])
    assert shipped == derived, 'table %s, image %s' % (
        sorted(hex(v) for f in sorted(shipped) for v in f),
        sorted(hex(v) for f in sorted(derived) for v in f))
    print('  matches jni/src/loot_memory.h (%d flows)' % len(found['flows']))
    # Row order is NOT derived: it decides the reported Kind, so it stays a
    # judgement call against dump.cs. Print both orders side by side.
    shipped_order = [tuple(f) for f in row['flows']]
    if shipped_order != found['flows']:
        print('  note: the table lists them in Kind order %s' %
              [hex(f[0]) for f in shipped_order])


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '--check']
    do_check = '--check' in sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    layouts, _prologue = parse_table(HEADER.read_text()) if do_check else (None, None)
    for path in args:
        found = derive(path)
        assert len(found['flows']) == 3, \
            '%s: %d stock awaits found, expected 3 — inspect them by hand' % (
                path, len(found['flows']))
        report(path, found)
        if do_check:
            check(path, found, layouts)
    print('loot: table re-derived from the image bytes')

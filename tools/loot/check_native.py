#!/usr/bin/env python3
"""Optional ARM64 checks of the instant-pickup table against the game images.

Requires capstone. No game images are checked in; extract them first (see
tools/offsets/PROVENANCE.md):

    bash tools/offsets/extract_dumps.sh /tmp/dump        # release
    python3 -c "import py7zr; py7zr.SevenZipFile('dump_beta.7z').extractall('/tmp/beta')"

    check_native.py release/libil2cpp.so beta/libil2cpp.so

The controller never executes game code and never writes to the images, so what
has to be true is narrower than for an instruction patch — but it still has to
be true, and it is re-derived here from the shipped bytes instead of being
trusted from the research notes:

  * build-id at 0x318, the only key the layout table is selected by;
  * AsyncUniTask`1<...>::Run — the four prologue words the identity chain
    matches on, decoded: they read the delegate's m_target at +0x20 and take
    &stateMachine at runner+0x20, the two runtime offsets the chain relies on;
  * the tail branch of Run lands exactly on the table's move_next;
  * each delay site still holds its ORIGINAL `ldr s0,[x8,#imm]` (so a process
    carrying the retired instruction patch is refused, not matched), the loaded
    literal is 0.35 s, and the call it feeds is UniTask.WaitForSeconds with
    ignoreTimeScale = true and PlayerLoopTiming.Update = 8;
  * all three flows await through the same helper;
  * PlayerLoopHelper resolves through the table's TypeInfo slot: the game's own
    PlayerLoopHelper.AddAction reaches that slot through one pointer, and then
    uses static_fields at +0xB8, runners at +0x30 and the array length at +0x18
    — the same three offsets the controller reads.

This validates addresses and arithmetic against the images. It says nothing
about live Android ownership, server acceptance or anti-cheat safety.
"""
import mmap
import re
import struct
import sys
from pathlib import Path

try:
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
except ImportError as exc:                                  # pragma: no cover
    raise SystemExit('capstone is required: pip install capstone (%s)' % exc)

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / 'jni/src/loot_memory.h'

# Verification-only anchor: PlayerLoopHelper.AddAction, the game's own code that
# resolves PlayerLoopHelper. Nothing in jni/ depends on it; it is here so the
# TypeInfo slot in the table can be re-derived instead of trusted.
ADD_ACTION = [0xc322340, 0xc3048fc]            # release, beta
DELAY_SECONDS = 0.35                           # the stock per-item delay
R_AARCH64_RELATIVE = 0x403

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
md.detail = True


def parse_table(text):
    """Read the constants the controller ships with, in table order."""
    prologue = text.split('kRunPrologue[4] = {', 1)[1].split('};', 1)[0]
    prologue = [int(m, 16) for m in re.findall(r'0x([0-9a-fA-F]+)', prologue)]
    assert len(prologue) == 4, prologue
    body = text.split('inline constexpr Layout kLayouts[] = {', 1)[1].split('\n};', 1)[0]
    nums = [int(m, 16) for m in re.findall(r'0x([0-9a-fA-F]+)', body)]
    assert nums and len(nums) % 45 == 0, 'unexpected layout table shape'
    layouts = []
    for start in range(0, len(nums), 45):
        chunk = nums[start:start + 45]
        build_id = bytes(chunk[:20])
        helper = chunk[20]
        flows = [tuple(chunk[21 + i * 8:29 + i * 8]) for i in range(3)]
        layouts.append({'build_id': build_id, 'helper': helper, 'flows': flows})
    return layouts, prologue


class Image:
    def __init__(self, path):
        with open(path, 'rb') as handle:
            self.data = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
        data = self.data
        assert data[:6] == b'\x7fELF\x02\x01', 'not an ARM64 ELF: %s' % path
        self.segments, self.sections = [], []
        phoff = struct.unpack_from('<Q', data, 0x20)[0]
        entsize, count = struct.unpack_from('<HH', data, 0x36)
        for i in range(count):
            kind, _flags, off, va, _pa, filesz, _memsz, _align = \
                struct.unpack_from('<II6Q', data, phoff + i * entsize)
            if kind == 1:
                self.segments.append((va, off, filesz))
        shoff = struct.unpack_from('<Q', data, 0x28)[0]
        shentsize, shcount, _shstrndx = struct.unpack_from('<HHH', data, 0x3a)
        for i in range(shcount):
            _nm, kind, _flags, _addr, off, size, _l, _inf, _al, _es = \
                struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize)
            self.sections.append((kind, off, size))

    def at(self, va, size):
        for start, off, filesz in self.segments:
            if start <= va and va + size <= start + filesz:
                return self.data[off + va - start:off + va - start + size]
        raise AssertionError('address %#x is not in any loaded segment' % va)

    def word(self, va):
        return struct.unpack('<I', self.at(va, 4))[0]

    def qword(self, va):
        return struct.unpack('<Q', self.at(va, 8))[0]

    def disasm(self, va, size):
        return list(md.disasm(self.at(va, size), va))

    def reloc(self, va):
        """(type, addend) of the dynamic relocation filling this slot."""
        for kind, off, size in self.sections:
            if kind != 4:                           # SHT_RELA
                continue
            for cursor in range(off, off + size, 24):
                r_off, r_info, r_add = struct.unpack_from('<QQq', self.data, cursor)
                if r_off == va:
                    return r_info & 0xffffffff, r_add
        return None


def check_flow(image, flow, prologue, calls):
    run, move_next, delay_site, delay_word, flag_site, call_site, flag_word, call_word = flow
    # Run(): the prologue the identity chain matches, decoded.
    words = [image.word(run + i * 4) for i in range(5)]
    assert words[:4] == prologue, 'Run prologue changed at %#x' % run
    ins = image.disasm(run, 20)
    assert len(ins) == 5
    assert ins[0].mnemonic == 'ldr' and ins[0].op_str == 'x8, [x1, #0x20]', ins[0].op_str
    assert ins[1].mnemonic == 'add' and ins[1].op_str == 'x0, x0, #0x20', ins[1].op_str
    assert ins[4].mnemonic == 'b'
    assert ins[4].operands[0].imm == move_next, \
        'Run %#x branches to %#x, table says %#x' % (run, ins[4].operands[0].imm, move_next)
    # The retired patch site must still hold its original literal load.
    assert image.word(delay_site) == delay_word, 'delay site %#x changed' % delay_site
    site = image.disasm(delay_site, 4)[0]
    assert site.mnemonic == 'ldr', 'delay site %#x is %s' % (delay_site, site.mnemonic)
    dest = md.reg_name(site.operands[0].reg)
    assert dest and dest[0] == 's', 'delay site %#x loads %s, not a float' % (delay_site, dest)
    assert site.operands[1].mem.base and site.operands[1].mem.disp >= 0
    before = image.disasm(delay_site - 24, 24)
    page = None
    args = []
    for ins_ in before:
        if ins_.mnemonic == 'adrp':
            page = ins_.operands[1].imm
        elif ins_.mnemonic == 'mov' and ins_.op_str.startswith('w'):
            args.append(ins_.op_str)
    assert page is not None, 'no adrp before the delay site %#x' % delay_site
    literal = page + site.operands[1].mem.disp
    value = struct.unpack('<f', image.at(literal, 4))[0]
    assert struct.unpack('<I', struct.pack('<f', value))[0] == \
        struct.unpack('<I', struct.pack('<f', DELAY_SECONDS))[0], \
        'delay site %#x loads %r, not %r' % (delay_site, value, DELAY_SECONDS)
    # UniTask.WaitForSeconds(seconds, ignoreTimeScale: true, PlayerLoopTiming.Update, ct)
    assert 'w0, #1' in args, 'ignoreTimeScale is no longer true at %#x' % delay_site
    assert 'w1, #8' in args, 'PlayerLoopTiming.Update is no longer 8 at %#x' % delay_site
    helper = None
    for ins_ in image.disasm(delay_site + 4, 24):
        if ins_.mnemonic == 'bl':
            helper = ins_.operands[0].imm
            break
    assert helper is not None, 'no call after the delay site %#x' % delay_site
    calls.add(helper)
    # Две инструкции вызова задержки — мишень нашего патча: `mov w0, #1` сразу
    # после adrp (за 8 байт до ldr) и сам `bl WaitForSeconds`. Таблица обязана
    # держать точные RVA и слова: иначе патч молча попадёт мимо.
    assert flag_site == delay_site - 8, 'flag site %#x is not delay-8 (%#x)' % (flag_site, delay_site)
    assert call_site == delay_site + 0x10, 'call site %#x is not delay+0x10 (%#x)' % (call_site, delay_site)
    assert image.word(flag_site) == flag_word == 0x52800020, \
        'flag site %#x holds %#x' % (flag_site, image.word(flag_site))
    assert image.word(call_site) == call_word, \
        'call site %#x holds %#x, table says %#x' % (call_site, image.word(call_site), call_word)
    call = image.disasm(call_site, 4)[0]
    assert call.mnemonic == 'bl' and call.operands[0].imm == helper, \
        'call site %#x is %s %s' % (call_site, call.mnemonic, call.op_str)
    return ('Run %#x -> MoveNext %#x (m_target +0x20, state machine +0x20); '
            'delay %#x loads %.2f s, ignoreTimeScale + Update(8), awaits %#x'
            % (run, move_next, delay_site, value, helper))


def check_helper(image, helper_slot, add_action):
    """Re-derive the TypeInfo slot from the game's own PlayerLoopHelper code."""
    ins = image.disasm(add_action, 0x60)
    page, slot = None, None
    for i in ins:
        if i.mnemonic == 'adrp':
            page = i.operands[1].imm
        elif i.mnemonic == 'ldr' and page is not None and slot is None and \
                i.operands[1].mem.base and i.op_str.startswith('x21'):
            slot = page + i.operands[1].mem.disp
    assert slot is not None, 'AddAction %#x does not resolve a class pointer' % add_action
    kind, addend = image.reloc(slot) or (None, None)
    assert kind == R_AARCH64_RELATIVE, 'slot %#x is not a relative reloc' % slot
    assert addend == helper_slot, \
        'AddAction reaches %#x, the table says %#x' % (addend, helper_slot)
    # The same code then walks static_fields -> runners -> array length.
    text = [i.op_str for i in ins]
    assert 'x8, [x0, #0xb8]' in text, 'static_fields is no longer at +0xB8'
    assert 'x8, [x8, #0x30]' in text, 'runners is no longer at +0x30'
    assert 'w9, [x8, #0x18]' in text, 'the array length is no longer at +0x18'
    assert 'x0, [x21]' in text, 'AddAction no longer reads the class pointer'
    return 'AddAction %#x -> slot %#x -> TypeInfo %#x (statics +0xB8, runners +0x30)' % (
        add_action, slot, helper_slot)


def check(path, layout, prologue, add_action):
    image = Image(path)
    assert image.at(0x318, 20) == layout['build_id'], \
        '%s: build-id %s, the table expects %s' % (
            path, image.at(0x318, 20).hex(), layout['build_id'].hex())
    calls = set()
    lines = [check_flow(image, flow, prologue, calls) for flow in layout['flows']]
    assert len(calls) == 1, 'the three flows await through %s' % sorted(hex(c) for c in calls)
    lines.append(check_helper(image, layout['helper'], add_action))
    lines.append('all three flows await through UniTask.WaitForSeconds %#x' % calls.pop())
    print('%s: build-id %s OK' % (path, layout['build_id'].hex()))
    for line in lines:
        print('  ' + line)


if __name__ == '__main__':
    layouts, prologue = parse_table(HEADER.read_text())
    if len(sys.argv) != 1 + len(layouts):
        raise SystemExit(__doc__)
    for path, layout, add_action in zip(sys.argv[1:], layouts, ADD_ACTION):
        check(path, layout, prologue, add_action)
    print('loot: every address in jni/src/loot_memory.h matches the shipped images')

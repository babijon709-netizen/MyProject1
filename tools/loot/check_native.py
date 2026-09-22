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
    — the same three offsets the controller reads;
  * DelayIgnoreTimeScalePromise: MoveNext really accumulates into [obj+0x1C]
    against [obj+0x18], and TryReturn zeroes both floats with one xzr store on
    the way back to the pool — the evidence for treating a dead/zeroed timer
    as "work is finished", not as a refusal.

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
# Методы, из которых выводятся DI-анкеры детекта окна (script.json обоих билдов):
# Zenject.ProjectContext.get_Instance и .cctor класса id окон (ms / qn).
DI_GET_INSTANCE = [0xcccf804, 0xccb1dc0]       # release, beta
DI_IDS_CCTOR    = [0x73ce78c, 0x735bfd4]       # release, beta
# DelayIgnoreTimeScalePromise: MoveNext и TryReturn (dump.cs обоих билдов).
# MoveNext доказывает раскладку таймера (elapsed +0x1C, duration +0x18), а
# TryReturn — зануление обеих float одной записью xzr при возврате в пул: отсюда
# правило «таймер занулён/вне окна = конец работы, не отказ».
PROMISE_OFFSETS = [(0xc32f8f4, 0xc32f9f4), (0xc311eb0, 0xc311fb0)]   # release, beta
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
    # Строка: 20 байт build-id + helper + projctx + ids + 3 потока по 8 чисел.
    assert nums and len(nums) % 47 == 0, 'unexpected layout table shape'
    names = re.findall(r'"([A-Za-z][A-Za-z0-9]{1,3})"', body)
    layouts = []
    for i, start in enumerate(range(0, len(nums), 47)):
        chunk = nums[start:start + 47]
        build_id = bytes(chunk[:20])
        helper, projctx, ids = chunk[20], chunk[21], chunk[22]
        flows = [tuple(chunk[23 + j * 8:31 + j * 8]) for j in range(3)]
        layouts.append({'build_id': build_id, 'helper': helper, 'projctx': projctx,
                        'ids': ids, 'flows': flows,
                        'iface': names[2 * i], 'service': names[2 * i + 1]})
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


def _slots_with_addend(image, ins, addend):
    """Все (addrp; ldr [addrp+off]) слоты, чья релокация указывает на addend."""
    hits, page = [], None
    for i in ins:
        if i.mnemonic == 'adrp':
            page = i.operands[1].imm
        elif i.mnemonic == 'ldr' and page is not None and i.operands[1].mem.base:
            slot = page + i.operands[1].mem.disp
            rel = image.reloc(slot)
            if rel and rel[0] == R_AARCH64_RELATIVE and rel[1] == addend:
                hits.append(slot)
            page = None
    return hits


def check_di(image, layout, get_instance, ids_cctor):
    """DI-анкеры детекта окна выводятся из кода образа, не из таблицы."""
    ins = image.disasm(get_instance, 0xB0)
    hits = _slots_with_addend(image, ins, layout['projctx'])
    assert hits, 'get_Instance %#x: нет слота на TypeInfo %#x' % (
        get_instance, layout['projctx'])
    text = [i.op_str for i in ins]
    assert 'x8, [x8, #0xb8]' in text, \
        'get_Instance %#x: static_fields ушёл с +0xB8' % get_instance
    assert any(op.startswith(('x8, [x8, #0x20]', 'x19, [x8, #0x20]')) for op in text), \
        'get_Instance %#x: _instance ушёл с +0x20' % get_instance
    lines = ['ProjectContext.get_Instance %#x -> slot %#x -> TypeInfo %#x '
             '(statics +0xB8, _instance +0x20)' % (get_instance, hits[0], layout['projctx'])]
    ins = image.disasm(ids_cctor, 0x120)
    hits = _slots_with_addend(image, ins, layout['ids'])
    assert hits, 'ids .cctor %#x: нет слота на TypeInfo %#x' % (
        ids_cctor, layout['ids'])
    text = [i.op_str for i in ins]
    assert any('#0xb8' in op for op in text), \
        'ids .cctor %#x: статики не через +0xB8' % ids_cctor
    assert any(i.mnemonic in ('str', 'stp', 'strb', 'strh', 'stur') for i in ins), \
        'ids .cctor %#x: id окон не пишутся в статики' % ids_cctor
    lines.append('%s.cctor %#x -> slot %#x -> TypeInfo %#x (161 id в статиках)' % (
        layout['service'], ids_cctor, hits[0], layout['ids']))
    return lines


def check_promise(image, move_next, try_return):
    """DelayIgnoreTimeScalePromise: timer layout and the pool's zero-write."""
    ldr_elapsed = fadd = ldr_duration = fcmp = str_elapsed = None
    order = []
    for ins in image.disasm(move_next, 120 * 4):
        op = ins.op_str
        float_load = ins.mnemonic == 'ldr' and op.startswith('s') and op.endswith(']')
        float_store = ins.mnemonic == 'str' and op.startswith('s') and op.endswith(']')
        if float_load and op.endswith('#0x1c]'):
            ldr_elapsed = ins; order.append('L1C')
        elif float_load and op.endswith('#0x18]'):
            ldr_duration = ins; order.append('L18')
        elif ins.mnemonic == 'fadd':
            fadd = ins; order.append('ADD')
        elif ins.mnemonic == 'fcmp':
            fcmp = ins; order.append('CMP')
        elif float_store and op.endswith('#0x1c]'):
            str_elapsed = ins; order.append('S1C'); break
        elif ins.mnemonic == 'ret':
            break
    # Порядок операций должен совпасть с dump-семантикой: накопление dt в
    # elapsed через fadd читается до записи [+0x1c], compare идёт против [+0x18].
    assert 'L1C' in order and 'ADD' in order and 'L18' in order and 'CMP' in order \
        and 'S1C' in order, \
        'MoveNext %#x: the elapsed/duration float pattern is gone (%s)' \
        % (move_next, order)
    assert order.index('ADD') < order.index('S1C') and \
        order.index('L18') < order.index('S1C'), \
        'MoveNext %#x: timer operations reordered' % move_next
    assert ldr_elapsed and fadd and ldr_duration and fcmp and str_elapsed
    zero_write = None
    for ins in image.disasm(try_return, 60 * 4):
        if ins.mnemonic in ('str', 'stur') and ins.op_str.startswith('xzr') and \
                ins.op_str.endswith('#0x18]'):
            zero_write = ins
            break
        if ins.mnemonic == 'ret':
            break
    assert zero_write, 'TryReturn %#x: the pool no longer zeroes the timer pair' % try_return
    return ('promise %#x: MoveNext adds to [+0x1c], compares to [+0x18]; '
            'TryReturn %#x zeroes the pair, so a dead timer means completed, not failed'
            % (move_next, try_return))


def check(path, layout, prologue, add_action, get_instance, ids_cctor, promise):
    image = Image(path)
    assert image.at(0x318, 20) == layout['build_id'], \
        '%s: build-id %s, the table expects %s' % (
            path, image.at(0x318, 20).hex(), layout['build_id'].hex())
    calls = set()
    lines = [check_flow(image, flow, prologue, calls) for flow in layout['flows']]
    assert len(calls) == 1, 'the three flows await through %s' % sorted(hex(c) for c in calls)
    lines.append(check_helper(image, layout['helper'], add_action))
    lines.extend(check_di(image, layout, get_instance, ids_cctor))
    lines.append(check_promise(image, *promise))
    lines.append('all three flows await through UniTask.WaitForSeconds %#x' % calls.pop())
    print('%s: build-id %s OK' % (path, layout['build_id'].hex()))
    for line in lines:
        print('  ' + line)


if __name__ == '__main__':
    layouts, prologue = parse_table(HEADER.read_text())
    if len(sys.argv) != 1 + len(layouts):
        raise SystemExit(__doc__)
    for path, layout, aa, gi, cc, pr in zip(sys.argv[1:], layouts, ADD_ACTION,
                                            DI_GET_INSTANCE, DI_IDS_CCTOR,
                                            PROMISE_OFFSETS):
        check(path, layout, prologue, aa, gi, cc, pr)
    print('loot: every address in jni/src/loot_memory.h matches the shipped images')

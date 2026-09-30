#!/usr/bin/env python3
# Динамический вызов getsockopt пропатченного модуля: sweep по optname,
# трассировка сравнений с маркером -> список кодов команд.
import sys, struct, os
from unicorn import *
from unicorn.arm64_const import *
import capstone

sys.argv = [sys.argv[0]]  # ko_emu без аргументов
src = open('/tmp/drv/ko_emu.py').read()
# отрезаем хвост ko_emu после init (запуск/дамп/вывод) — оставляем только подготовку
cut = src.index("# --- запуск init_module ---")
head = src[:cut]
# выполняем подготовку (парсинг, релокации, память, хуки) в этом процессе
exec(compile(head, 'ko_emu_head', 'exec'), globals())

# --- собственно init ---
init_sec = next(i for i, s in enumerate(shs) if s['name'] == '.init.text')
init_addr = SECADDR[init_sec]
uc.reg_write(UC_ARM64_REG_SP, SP0)
uc.reg_write(UC_ARM64_REG_LR, MAGIC_STOP)
try:
    uc.emu_start(init_addr, MAGIC_STOP, timeout=60*1000000, count=100_000_000)
except UcError as e:
    L(f'init UC ERROR: {e}')

text_i = next(i for i, s in enumerate(shs) if s['name'] == '.text')
TA = SECADDR[text_i]

# --- фейковые структуры ---
FAKE_SOCK = 0x600001000       # struct socket
FAKE_SK   = 0x600002000       # struct sock
uc.mem_map(0x600000000, 0x200000, UC_PROT_ALL)
OPTVAL = 0x600003000
OPTLEN = 0x600004000
# socket: ...->sk at offset 0x20 (struct socket { short type; ... struct sock *sk; }) — заполним несколько полей
uc.mem_write(FAKE_SOCK + 0x20, struct.pack('<Q', FAKE_SK))
# sk: sk_prot (offset 0x28 в 5.10? варьируется) — заполним нулями, но sk_user_data
# переберём: сначала нули

MARKER = 0x5AA5A5
import collections
trace = []
cur_trace = []

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
ins_cache = {}
def get_ins(pc):
    if pc not in ins_cache:
        try:
            code = bytes(uc.mem_read(pc, 4))
            i = next(md.disasm(code, pc))
            ins_cache[pc] = (i.mnemonic, i.op_str)
        except StopIteration:
            ins_cache[pc] = ('?', '?')
    return ins_cache[pc]

def hook_code(uc, address, size, ud):
    mn, ops = get_ins(address)
    if mn in ('cmp', 'subs', 'cmn', 'tst'):
        # соберём значения регистров-операндов
        import re as _re
        regs = _re.findall(r'\b[wx](\d{1,2})\b', ops)
        import unicorn.arm64_const as _ac
        vals = {}
        for rn in regs:
            idx = int(rn)
            if idx > 30: continue
            vals[idx] = uc.reg_read(getattr(_ac, f'UC_ARM64_REG_X{idx}'))
        cur_trace.append(('cmp', address, ops, vals))
    elif mn.startswith('b.') or mn == 'csel' or mn == 'csinc':
        cur_trace.append(('br', address, mn, ops))

uc.hook_add(UC_HOOK_CODE, hook_code, begin=TA, end=TA + 0x3400)

blk_log = []
def hook_blk_all(uc, addr, size, ud):
    blk_log.append(addr)
uc.hook_add(UC_HOOK_BLOCK, hook_blk_all)

def call_getsockopt(level, optname):
    global cur_trace
    cur_trace = []
    uc.mem_write(OPTLEN, struct.pack('<i', 8))
    uc.mem_write(OPTVAL, b'\x00' * 16)
    uc.reg_write(UC_ARM64_REG_SP, SP0)
    uc.reg_write(UC_ARM64_REG_LR, MAGIC_STOP)
    uc.reg_write(UC_ARM64_REG_X0, FAKE_SOCK)
    uc.reg_write(UC_ARM64_REG_X1, level)
    uc.reg_write(UC_ARM64_REG_X2, optname)
    uc.reg_write(UC_ARM64_REG_X3, OPTVAL)
    uc.reg_write(UC_ARM64_REG_X4, OPTLEN)
    ret_insns = 0
    try:
        uc.emu_start(TA + 0x32e0, MAGIC_STOP, timeout=30*1000000, count=30_000_000)  # .NMSL43 thunk
        ret_insns = uc.reg_read(UC_ARM64_REG_X0)
    except UcError as e:
        return None, str(e), cur_trace
    return ret_insns, None, cur_trace

# 1) прогон с маркером — найти сравнения optname
blk_log.clear()
ret, err, tr = call_getsockopt(1234, MARKER)
print('БЛОКИ:', ' '.join(hex(a) for a in blk_log[:60]), file=sys.stderr)
print(f'getsockopt(level=1234, opt={MARKER:#x}) -> ret={ret} err={err}, трасса {len(tr)}', file=sys.stderr)
markercmps = []
for kind, pc, ops, vals in tr:
    if kind == 'cmp':
        has_marker = any(v == MARKER for v in vals.values())
        if has_marker:
            markercmps.append((pc, ops, vals))
print(f'сравнений с маркером: {len(markercmps)}', file=sys.stderr)
for pc, ops, vals in markercmps[:40]:
    print(f'  {pc:#x} (+{pc-TA:#x}): {ops} vals={ {k: hex(v) for k, v in vals.items()} }', file=sys.stderr)

# setsockopt-тест
def call_setsockopt(level, optname, optval=b''):
    global cur_trace
    cur_trace = []
    uc.mem_write(OPTLEN, struct.pack('<i', len(optval) or 8))
    uc.mem_write(OPTVAL, (optval or b'\x00'*8).ljust(16, b'\x00'))
    uc.reg_write(UC_ARM64_REG_SP, SP0)
    uc.reg_write(UC_ARM64_REG_LR, MAGIC_STOP)
    uc.reg_write(UC_ARM64_REG_X0, FAKE_SOCK)
    uc.reg_write(UC_ARM64_REG_X1, level)
    uc.reg_write(UC_ARM64_REG_X2, optname)
    uc.reg_write(UC_ARM64_REG_X3, OPTVAL)
    uc.reg_write(UC_ARM64_REG_X4, OPTLEN)
    blk_log.clear()
    try:
        uc.emu_start(TA + 0x32e8, MAGIC_STOP, timeout=30*1000000, count=30_000_000)
        ret = uc.reg_read(UC_ARM64_REG_X0)
    except UcError as e:
        return None, str(e), cur_trace
    return ret, None, cur_trace

blk_log.clear()
ret, err, tr = call_setsockopt(1234, MARKER)
print('=== setsockopt(1234, MARKER) ===', file=sys.stderr)
print('ret =', hex(ret) if ret is not None else ret, 'err =', err, file=sys.stderr)
print('БЛОКИ:', ' '.join(hex(a) for a in blk_log[:80]), file=sys.stderr)
print(f'сравнений: {len(tr)}', file=sys.stderr)
mm = [e for e in tr if e[0]=='cmp' and any(v == MARKER for v in e[3].values())]
print(f'с маркером: {len(mm)}', file=sys.stderr)
for pc, ops, vals in [e[1:] for e in mm][:30]:
    print(f'  {pc:#x} (+{pc-TA:#x}): {ops} vals={ {k: hex(v) for k, v in vals.items()} }', file=sys.stderr)
for kind, pc, ops, vals in tr[:20]:
    print(f'  first: {pc:#x} {kind} {ops} { {k: hex(v) for k, v in vals.items()} if vals else ""}', file=sys.stderr)

# ioctl-тест: .NMSL46 @ .text+0x3300
def call_ioctl(fd_cmd, arg):
    global cur_trace
    cur_trace = []
    uc.reg_write(UC_ARM64_REG_SP, SP0)
    uc.reg_write(UC_ARM64_REG_LR, MAGIC_STOP)
    uc.reg_write(UC_ARM64_REG_X0, FAKE_SOCK)
    uc.reg_write(UC_ARM64_REG_X1, fd_cmd)
    uc.reg_write(UC_ARM64_REG_X2, arg)
    blk_log.clear()
    try:
        uc.emu_start(TA + 0x3300, MAGIC_STOP, timeout=30*1000000, count=30_000_000)
        ret = uc.reg_read(UC_ARM64_REG_X0)
    except UcError as e:
        return None, str(e), cur_trace
    return ret, None, cur_trace

blk_log.clear()
ret, err, tr = call_ioctl(MARKER, OPTVAL)
print('=== ioctl(MARKER) ===', file=sys.stderr)
print('ret =', hex(ret) if ret is not None else ret, 'err =', err, file=sys.stderr)
print('БЛОКИ:', ' '.join(hex(a) for a in blk_log[:100]), file=sys.stderr)
print(f'сравнений: {len(tr)}', file=sys.stderr)
mm = [e for e in tr if e[0]=='cmp' and any(v == MARKER for v in e[3].values())]
print(f'с маркером: {len(mm)}', file=sys.stderr)
for e in mm[:40]:
    pc, ops, vals = e[1], e[2], e[3]
    print(f'  {pc:#x} (+{pc-TA:#x}): {ops} vals={ {k: hex(v) for k, v in vals.items()} }', file=sys.stderr)
if not mm:
    for kind, pc, ops, vals in tr[:25]:
        vv = vals if isinstance(vals, dict) else {}
        print(f'  first {pc:#x}: {kind} {ops} { {k: hex(v) for k, v in vv.items()} }', file=sys.stderr)

# === CREATE + IOCTL ===
# net_proto_family.create(net, sock, protocol, kern); sock->type short @ +0x4? struct socket: state(int?), type(short)
def call_create(sock_type, protocol, kern=0):
    global cur_trace
    cur_trace = []
    uc.mem_write(FAKE_SOCK, b'\x00' * 0x40)
    uc.mem_write(FAKE_SOCK + 0x0, struct.pack('<I', 0))       # state SS_UNCONNECTED
    uc.mem_write(FAKE_SOCK + 0x4, struct.pack('<h', sock_type))
    uc.mem_write(FAKE_SOCK + 0x20, struct.pack('<Q', FAKE_SK))
    uc.mem_write(FAKE_SK, b'\x00' * 0x100)
    uc.reg_write(UC_ARM64_REG_SP, SP0)
    uc.reg_write(UC_ARM64_REG_LR, MAGIC_STOP)
    uc.reg_write(UC_ARM64_REG_X0, 0x600005000)  # net (фейк)
    uc.reg_write(UC_ARM64_REG_X1, FAKE_SOCK)
    uc.reg_write(UC_ARM64_REG_X2, protocol)
    uc.reg_write(UC_ARM64_REG_X3, kern)
    blk_log.clear()
    try:
        uc.emu_start(TA + 0x3340, MAGIC_STOP, timeout=30*1000000, count=30_000_000)  # .NMSL67 (create)
        ret = uc.reg_read(UC_ARM64_REG_X0)
    except UcError as e:
        return None, str(e), cur_trace
    return ret, None, cur_trace

for t, p in ((6, 0), (4, 0), (2, 0), (6, 0x90419), (4, 0x90419), (3, 0), (5, 0)):
    ret, err, tr = call_create(t, p)
    print(f'create(type={t}, proto={p:#x}) -> ret={None if ret is None else hex(ret & 0xffffffff)} err={err} '
          f'блоки={len(blk_log)}', file=sys.stderr)

# === create(4) затем ioctl ===
ret, err, tr = call_create(4, 0)
print(f'create(4,0) -> {None if ret is None else hex(ret & 0xffffffff)}', file=sys.stderr)
for code in (0x300, 0x301, 0x302, 0x303):
    uc.mem_write(OPTVAL, struct.pack('<QQQQ', 1234, 0x7f00001000, 16, 0x41414141))
    ret, err, tr = call_ioctl(code, OPTVAL)
    calls = [e for e in tr if isinstance(e, tuple) and e[0] in ('call', 'copy_to_user')]
    print(f'--- после create: ioctl({code:#x}) ret={None if ret is None else hex(ret & 0xffffffff)} err={err} блоков={len(blk_log)}', file=sys.stderr)
    for e in calls[:12]:
        print(f'    {e}', file=sys.stderr)
    out = bytes(uc.mem_read(OPTVAL, 32))
    print(f'    optval: {out.hex()}', file=sys.stderr)

# === кто пишет [sp+0x24] при ioctl(0x300) ===
writes_log = []
def hook_writes(uc, access, address, size, value, ud):
    sp = uc.reg_read(UC_ARM64_REG_SP)
    if sp - 0x80 <= address <= sp + 0x40:
        writes_log.append((address - sp, size, value, uc.reg_read(UC_ARM64_REG_PC)))
h_w = uc.hook_add(UC_HOOK_MEM_WRITE, hook_writes)
uc.mem_write(OPTVAL, struct.pack('<QQQQ', 1234, 0x7f00001000, 16, 0x41414141))
ret, err, tr = call_ioctl(0x300, OPTVAL)
uc.hook_del(h_w)
print('=== записи в кадр стека при ioctl(0x300) ===', file=sys.stderr)
for off, size, value, pc in writes_log:
    if off == 0x24 or value in (0, 1):
        print(f'  [sp{off:+#x}] = {value:#x} (size {size}) от pc={pc:#x} (+{pc-TA:#x})', file=sys.stderr)

# === имена импортов и правдоподобные объекты ===
print('импорты 34..50:', {k: import_names[k] for k in range(34, min(50, len(import_names)))}, file=sys.stderr)
FAKE_PID = 0x600010000
FAKE_TASK = 0x600011000
FAKE_MM = 0x600012000
FAKE_VMA = 0x600013000
uc.mem_write(FAKE_VMA, struct.pack('<QQQQ', 0x7000000000, 0x7100000000, 0, 0))  # vm_start, vm_end...

_orig_intr = None
def hook_intr2(uc, intno, data):
    x16 = uc.reg_read(UC_ARM64_REG_X16)
    x0 = uc.reg_read(UC_ARM64_REG_X0)
    x1 = uc.reg_read(UC_ARM64_REG_X1)
    x2 = uc.reg_read(UC_ARM64_REG_X2)
    if 0 <= x16 < len(IMPORTS):
        name = import_names[x16]
        ret = None
        if name == 'find_get_pid':
            ret = FAKE_PID
        elif name == 'get_pid_task':
            ret = FAKE_TASK
        elif name == 'get_task_mm':
            ret = FAKE_MM
        elif name == 'find_vma':
            ret = FAKE_VMA
        elif name in ('put_pid', 'mmput', '__put_task_struct', 'up_read', 'down_read'):
            ret = 0
        if ret is not None:
            cur_trace.append(('call', name, x0, x1, x2))
            uc.reg_write(UC_ARM64_REG_X0, ret)
            return
    return hook_intr(uc, intno, data)
# подменяем хук
global _orig_hook_intr
uc.hook_del(uc._hook_intr_handle) if hasattr(uc, '_hook_intr_handle') else None
import types
# проще: добавим новый хук — unicorn вызывает все; наш вернёт раньше только для подменённых
def wrap_intr(uc, intno, data):
    hook_intr2(uc, intno, data)
uc.hook_add(UC_HOOK_INTR, wrap_intr)

# === current (sp_el0) для copy_from_user ===
uc.reg_write(UC_ARM64_REG_SP_EL0, FAKE_TASK)
uc.mem_write(FAKE_TASK + 0x00, struct.pack('<Q', 0))            # thread_info.flags
uc.mem_write(FAKE_TASK + 0x08, struct.pack('<Q', 0x1000000000000))  # addr_limit (48-бит VA)
uc.mem_write(FAKE_TASK + 0x46, b'\x00')
print('sp_el0 =', hex(uc.reg_read(UC_ARM64_REG_SP_EL0)), file=sys.stderr)

# === .text[0] чётный (как после патча ядром) ===
print('.text[0] до:', bytes(uc.mem_read(TA, 8)).hex(), file=sys.stderr)
uc.mem_write(TA, bytes([0x3e]) + bytes(uc.mem_read(TA+1, 15)))  # сделать чётным
for code in (0x300, 0x301, 0x302, 0x303):
    buf = bytearray(49)
    struct.pack_into('<Q', buf, 0, 1234)
    struct.pack_into('<Q', buf, 8, 0x7f00001000)
    struct.pack_into('<Q', buf, 16, 16)
    struct.pack_into('<I', buf, 32, 0x1337)
    uc.mem_write(OPTVAL, bytes(buf))
    ret, err, tr = call_ioctl(code, OPTVAL)
    calls = [e for e in tr if isinstance(e, tuple) and e[0] in ('call', 'copy_to_user')]
    print(f'ioctl({code:#x}) text0-even: ret={None if ret is None else hex(ret & 0xffffffff)} блоков={len(blk_log)}', file=sys.stderr)
    for e in calls[:12]:
        print(f'    {e}', file=sys.stderr)
    out = bytes(uc.mem_read(OPTVAL, 49))
    print(f'    buf: {out.hex()}', file=sys.stderr)

# === копирование "не удалось" (res = size) ===
def set_copy_fail(fail):
    global _copy_fail
    _copy_fail = fail
global _copy_fail
_copy_fail = False
# патчим заглушку через глобальный флаг: перехват в wrap_intr
def hook_intr3(uc, intno, data):
    x16 = uc.reg_read(UC_ARM64_REG_X16)
    if 0 <= x16 < len(IMPORTS) and import_names[x16] == '__arch_copy_from_user' and _copy_fail:
        uc.reg_write(UC_ARM64_REG_X0, uc.reg_read(UC_ARM64_REG_X2))
        return
    return wrap_intr(uc, intno, data)
uc.hook_add(UC_HOOK_INTR, hook_intr3)
for cf in (False, True):
    _copy_fail = cf
    buf = bytearray(49)
    struct.pack_into('<Q', buf, 0, 1234)
    struct.pack_into('<I', buf, 32, 0x1337)
    uc.mem_write(OPTVAL, bytes(buf))
    ret, err, tr = call_ioctl(0x300, OPTVAL)
    print(f'ioctl(0x300) copy_fail={cf}: ret={None if ret is None else hex(ret & 0xffffffff)} блоков={len(blk_log)}', file=sys.stderr)
    print('  путь:', ' '.join(hex(b) for b in blk_log[:45]), file=sys.stderr)
_copy_fail = False

# === с магией 0x1337 на +32 ===
for code in (0x300, 0x301, 0x302, 0x303):
    buf = bytearray(49)
    struct.pack_into('<Q', buf, 0, 1234)          # pid
    struct.pack_into('<Q', buf, 8, 0x7f00001000)  # addr
    struct.pack_into('<Q', buf, 16, 16)           # size
    struct.pack_into('<I', buf, 32, 0x1337)       # МАГИЯ
    uc.mem_write(OPTVAL, bytes(buf))
    ret, err, tr = call_ioctl(code, OPTVAL)
    calls = [e for e in tr if isinstance(e, tuple) and e[0] in ('call', 'copy_to_user')]
    print(f'ioctl({code:#x}) magic1337: ret={None if ret is None else hex(ret & 0xffffffff)} блоков={len(blk_log)}', file=sys.stderr)
    for e in calls[:10]:
        print(f'    {e}', file=sys.stderr)
    out = bytes(uc.mem_read(OPTVAL, 49))
    print(f'    buf: {out.hex()}', file=sys.stderr)

# === цепочка состояний x10 на диспетчере 0x1838 ===
states = []
def hook_state(uc, address, size, ud):
    states.append(uc.reg_read(UC_ARM64_REG_X10))
h_st = uc.hook_add(UC_HOOK_CODE, hook_state, begin=TA+0x1838, end=TA+0x1838)
uc.mem_write(OPTVAL, struct.pack('<QQQQ', 1234, 0x7f00001000, 16, 0x41414141) + b'\xAA' * 17)
ret, err, tr = call_ioctl(0x300, OPTVAL)
uc.hook_del(h_st)
print('СОСТОЯНИЯ x10 @0x1838:', file=sys.stderr)
for s in states:
    print(f'   {s:#018x}', file=sys.stderr)

# === все команды с 49-байтным буфером ===
for code in (0x300, 0x301, 0x302, 0x303, 0x2ff, 0x304):
    uc.mem_write(OPTVAL, struct.pack('<QQQQ', 1234, 0x7f00001000, 16, 0x41414141) + b'\xAA' * 17)
    ret, err, tr = call_ioctl(code, OPTVAL)
    print(f'ioctl({code:#x}): ret={None if ret is None else hex(ret & 0xffffffff)} блоков={len(blk_log)}', file=sys.stderr)
    out = bytes(uc.mem_read(OPTVAL, 49))
    print(f'   buf: {out.hex()}', file=sys.stderr)

# === обращения к локальной структуре запроса ===
req_reads = []
def hook_reqread(uc, access, address, size, value, ud):
    x29 = uc.reg_read(UC_ARM64_REG_FP)
    if x29 - 0x48 <= address < x29 - 0x17 and size in (1, 2, 4, 8):
        req_reads.append((address - x29, size, uc.reg_read(UC_ARM64_REG_PC)))
h_rr = uc.hook_add(UC_HOOK_MEM_READ, hook_reqread)
uc.mem_write(OPTVAL, struct.pack('<QQQQ', 1234, 0x7f00001000, 16, 0x41414141) + b'\xAA' * 17)
ret, err, tr = call_ioctl(0x300, OPTVAL)
uc.hook_del(h_rr)
print(f'=== обращения к req (ret={None if ret is None else hex(ret & 0xffffffff)}): {len(req_reads)} ===', file=sys.stderr)
import collections
cnt = collections.Counter((o, s) for o, s, p in req_reads)
for (o, s), c in sorted(cnt.items()):
    pcs = sorted(set(p for oo, ss, p in req_reads if (oo, ss) == (o, s)))
    print(f'  [x29{o:+#x}] size={s} x{c} от {[hex(p) for p in pcs[:4]]}', file=sys.stderr)

# === полная трасса ioctl(0x300) после create ===
ret, err, tr = call_create(4, 0)
import unicorn.arm64_const as _ac2
steps = []
def hook_steps(uc, address, size, ud):
    steps.append(address)
h_s = uc.hook_add(UC_HOOK_CODE, hook_steps, begin=TA, end=TA + 0x3400)
uc.mem_write(OPTVAL, struct.pack('<QQQQ', 1234, 0x7f00001000, 16, 0x41414141))
ret, err, tr = call_ioctl(0x300, OPTVAL)
uc.hook_del(h_s)
print(f'=== ПОЛНАЯ ТРАССА ioctl(0x300): {len(steps)} инстр, ret={None if ret is None else hex(ret & 0xffffffff)} ===', file=sys.stderr)
print('путь блоков:', ' '.join(hex(b) for b in blk_log[:60]), file=sys.stderr)
calls = [e for e in tr if isinstance(e, tuple) and e[0] in ('call', 'copy_to_user')]
print('вызовы:', file=sys.stderr)
for e in calls[:15]:
    print(f'  {e}', file=sys.stderr)
# значения стека на выходе
sp = uc.reg_read(UC_ARM64_REG_SP)
frame = bytes(uc.mem_read(sp, 0xa0))
print(f'кадр: {frame.hex()}', file=sys.stderr)

# === трассировка чтений при ioctl(0x300) ===
reads_log = []
def hook_reads(uc, access, address, size, value, ud):
    reads_log.append((address, size))
h_read = uc.hook_add(UC_HOOK_MEM_READ, hook_reads)
uc.mem_write(OPTVAL, struct.pack('<QQQQ', 1234, 0x7f00001000, 16, 0x41414141))
ret, err, tr = call_ioctl(0x300, OPTVAL)
uc.hook_del(h_read)
print(f'=== ioctl(0x300): ret={None if ret is None else hex(ret & 0xffffffff)}, чтений {len(reads_log)} ===', file=sys.stderr)
import collections
loclist = collections.Counter()
for a, s in reads_log:
    if 0x600000000 <= a < 0x600010000: loclist[(a - 0x600000000, s)] += 1
    elif 0x40000000 <= a < 0x40006000: loclist[('mod', a - 0x40000000, s)] += 1
for k, c in sorted(loclist.items(), key=lambda kv: str(kv[0])):
    print(f'  {k} x{c}', file=sys.stderr)
# и mrs-подобные? покажем вызовы
calls = [e for e in tr if isinstance(e, tuple) and e[0] in ('call', 'copy_to_user')]
for e in calls[:20]:
    print(f'  call: {e}', file=sys.stderr)

# === семантика команд 0x300..0x305 ===
for code in (0x300, 0x301, 0x302, 0x303, 0x304, 0x305):
    uc.mem_write(OPTVAL, struct.pack('<QQQQ', 1234, 0x7f00001000, 16, 0x41414141))
    ret, err, tr = call_ioctl(code, OPTVAL)
    calls = [e for e in tr if isinstance(e, tuple) and e[0] in ('call', 'copy_to_user')]
    print(f'--- ioctl({code:#x}) ret={None if ret is None else hex(ret & 0xffffffff)} err={err}', file=sys.stderr)
    for e in calls[:14]:
        print(f'    {e}', file=sys.stderr)
    out = bytes(uc.mem_read(OPTVAL, 32))
    print(f'    optval после: {out.hex()}', file=sys.stderr)

# === sweep 0..0xFFF по путям ===
print('=== SWEEP ===', file=sys.stderr)
import collections
groups = collections.defaultdict(list)
for code in range(0, 0x1000):
    uc.mem_write(OPTVAL, struct.pack('<QQQQ', 1234, 0x7f00001000, 16, 0x41414141))
    ret, err, tr = call_ioctl(code, OPTVAL)
    if ret is None:
        groups[('ERR', str(err)[:60])].append(code); continue
    key = (ret & 0xffffffff, tuple(sorted(set(blk_log))[:10]))
    groups[key].append(code)
for key, codes in sorted(groups.items(), key=lambda kv: kv[1][0]):
    if len(codes) <= 8:
        print(f'  ret={key[0]:#x} uniq-блоков={len(key[1])}: коды {[hex(c) for c in codes]}', file=sys.stderr)
    else:
        cont = codes == list(range(codes[0], codes[-1]+1))
        rng = f'{codes[0]:#x}..{codes[-1]:#x} ({len(codes)})' if cont else f'{len(codes)} кодов'
        print(f'  ret={key[0]:#x} uniq-блоков={len(key[1])}: {rng}', file=sys.stderr)

# 2) если маркер не найден — покажем первые 30 сравнений
if not markercmps:
    print('=== первые сравнения ===', file=sys.stderr)
    for kind, pc, ops, vals in tr[:30]:
        vv = vals if isinstance(vals, dict) else {}
        print(f'  {pc:#x} (+{pc-TA:#x}): {kind} {ops} vals={ {k: hex(v) for k, v in vv.items()} }', file=sys.stderr)
    print(f'ret={ret} err={err}', file=sys.stderr)
    # и блоки
    print('конец трассы:', file=sys.stderr)
    for kind, pc, ops, vals in tr[-20:]:
        print(f'  {pc:#x}: {kind} {ops}', file=sys.stderr)

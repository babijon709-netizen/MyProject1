#!/usr/bin/env python3
# Эмуляция init_module() из driver510.ko: применяем релокации, заглушаем импорты,
# прогоняем init, дампаем память (патчи thunks + расшифрованные строки + proto_ops).
import sys, struct, os
from unicorn import *
from unicorn.arm64_const import *
import capstone

KO = sys.argv[1] if len(sys.argv) > 1 else '/tmp/drv/ko_dump/driver510.ko'
blob = open(KO, 'rb').read()

e_shoff = struct.unpack('<Q', blob[40:48])[0]
e_shentsize, e_shnum, e_shstrndx = struct.unpack('<HHH', blob[58:64])
shs = []
for i in range(e_shnum):
    b = blob[e_shoff+i*e_shentsize:e_shoff+(i+1)*e_shentsize]
    nm, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack('<IIQQQQIIQQ', b)
    shs.append(dict(nm=nm, typ=typ, off=off, size=size, link=link, entsize=entsize, flags=flags))
shstr = blob[shs[e_shstrndx]['off']:shs[e_shstrndx]['off']+shs[e_shstrndx]['size']]
def sname(n):
    end = shstr.index(b'\x00', n); return shstr[n:end].decode()
for s in shs: s['name'] = sname(s['nm'])

# --- назначаем адреса секциям ---
BASE = 0x40000000            # .text
SECADDR = {}
cur = BASE
ALLOC = []  # (addr, size, shndx)
progbits = [i for i, s in enumerate(shs) if s['name'] in ('.text', '.rodata', '.data', '.init.text',
           '.exit.text', '.text.ftrace_trampoline', '.text..Lgoron_decrypt_string_i8', '.plt', '.init.plt')]
for i in progbits:
    s = shs[i]
    align = max(16, (s.get('align') if 'align' in s else 16) or 16)
    cur = (cur + 15) & ~15
    SECADDR[i] = cur
    ALLOC.append((cur, s['size'], i))
    cur += s['size']
# .bss тоже в адресной карте (символы на неё ссылаются!)
bss_i = next(i for i, s in enumerate(shs) if s['name'] == '.bss')
cur = (cur + 15) & ~15
BSS = SECADDR[bss_i] = cur
ALLOC.append((cur, max(shs[bss_i]['size'], 0x100), bss_i))
cur += max(shs[bss_i]['size'], 0x100)
# gnu.linkonce.this_module — тоже адресуем (init/exit указатели)
tm_i = next((i for i, s in enumerate(shs) if s['name'] == '.gnu.linkonce.this_module'), None)
if tm_i is not None:
    cur = (cur + 15) & ~15
    SECADDR[tm_i] = cur
    ALLOC.append((cur, shs[tm_i]['size'], tm_i))
    uc_mem_write_later = None
    cur += shs[tm_i]['size']
# __versions и прочие SHT_PROGBITS без имени-цели тоже, на всякий случай
for i, s in enumerate(shs):
    if i not in SECADDR and s['typ'] == 1 and s['size'] and s['name'] not in ('.modinfo', '.comment'):
        cur = (cur + 15) & ~15
        SECADDR[i] = cur
        ALLOC.append((cur, s['size'], i))
        cur += s['size']
MEMTOP = (cur + 0xfff) & ~0xfff

# --- symtab ---
symoff = shs[[i for i,s in enumerate(shs) if s['name']=='.symtab'][0]]
strtabsec = shs[symoff['link']]
strtab = blob[strtabsec['off']:strtabsec['off']+strtabsec['size']]
syms = []
for i in range(symoff['size'] // 24):
    b = blob[symoff['off']+i*24:symoff['off']+(i+1)*24]
    nameoff, info, other, shndx, value, size = struct.unpack('<IBBHQQ', b)
    end = strtab.index(b'\x00', nameoff)
    syms.append(dict(name=strtab[nameoff:end].decode(), value=value, size=size,
                     info=info, shndx=shndx))
def sym_addr(i):
    s = syms[i]
    if s['shndx'] == 0:  # импорт
        return None
    return SECADDR.get(s['shndx'], BASE) + s['value']

# --- память ---
uc = Uc(UC_ARCH_ARM64, UC_MODE_LITTLE_ENDIAN)
uc.mem_map(BASE, MEMTOP - BASE, UC_PROT_ALL)
for a, sz, i in ALLOC:
    s = shs[i]
    if sz: uc.mem_write(a, blob[s['off']:s['off']+s['size']])

# --- заглушки импортов ---
STUB = 0x41000000
uc.mem_map(STUB, 0x10000, UC_PROT_ALL)
IMPORTS = [i for i, s in enumerate(syms) if s['shndx'] == 0 and s['name']]
import_addr = {}
# ДАННЫЕ-импорты получают собственную память (не код-стабы!)
DATAIMPORTS = {  # имя: (размер, значение-заполнение)
    '__stack_chk_guard': (8, 0x9e3779b97f4a7c15),
    'vabits_actual': (4, 48),
    'memstart_addr': (8, 0),
    'gic_nonsecure_priorities': (4, 0),
    'cpu_hwcaps': (64, 0),          # без caps -> обычные пути uaccess
    'cpu_hwcap_keys': (64, 0),
    'arm64_const_caps_ready': (4, 1),  # капсы готовы
}
DATA_ADDR = 0x42000000
uc.mem_map(DATA_ADDR, 0x10000, UC_PROT_ALL)
data_next = DATA_ADDR
data_addrs = {}
for k, i in enumerate(IMPORTS):
    nm = syms[i]['name']
    if nm in DATAIMPORTS:
        sz, val = DATAIMPORTS[nm]
        a = data_next; data_next += max(sz, 16)
        data_addrs[nm] = a
        if sz == 8:
            uc.mem_write(a, struct.pack('<Q', val))
        else:
            uc.mem_write(a, struct.pack('<I', val))
        import_addr[i] = a
for k, i in enumerate(IMPORTS):
    if i in import_addr:
        continue
    a = STUB + k*16
    import_addr[i] = a
    code = struct.pack('<III', 0xd2800000 | ((k & 0xffff) << 5) | 16, 0xd4000001, 0xd65f03c0)
    uc.mem_write(a, code)
import_names = {k: syms[i]['name'] for k, i in enumerate(IMPORTS)}

# --- фейковые адреса объектов ядра (вне памяти модуля) ---
FAKE_SK_ADDR   = 0x600002000
FAKE_PID_ADDR  = 0x600006000
FAKE_TASK_ADDR = 0x600011000
FAKE_MM_ADDR   = 0x600012000
FAKE_VMA_ADDR  = 0x600013000
FAKE_BUF_ADDR  = 0x600004000
try:
    uc.mem_map(0x600000000, 0x200000, UC_PROT_ALL)
except UcError:
    pass

LOG = []
def L(msg):
    line = f'[{uc.reg_read(UC_ARM64_REG_PC):#x}] {msg}'
    LOG.append(line); print(line, file=sys.stderr)

# --- применяем релокации ---
def apply_relocs(rela_sh, target_sh):
    if rela_sh is None: return
    tgt = SECADDR.get(target_sh)
    if tgt is None: return
    for k in range(rela_sh['size'] // 24):
        r_off, r_info, r_add = struct.unpack('<QQq', blob[rela_sh['off']+k*24:rela_sh['off']+k*24+24])
        rtype = r_info & 0xffffffff
        symi = r_info >> 32
        S = sym_addr(symi)
        if S is None:
            S = import_addr.get(symi, 0)
        place = tgt + r_off
        old = struct.unpack('<I', bytes(uc.mem_read(place, 4)))[0]
        def s21(v):  # 21-битное знаковое
            return v & 0x1fffff
        if rtype == 257:  # ABS64
            uc.mem_write(place, struct.pack('<Q', (S + r_add) & (2**64-1)))
        elif rtype == 1027:  # RELATIVE
            uc.mem_write(place, struct.pack('<Q', r_add & (2**64-1)))
        elif rtype == 275:  # ADR_PREL_PG_HI21
            page = ((S + r_add) & ~0xfff) - (place & ~0xfff)
            imm = page >> 12
            immlo = imm & 3; immhi = (imm >> 2) & 0x7ffff
            if imm >= (1 << 20) or imm < -(1 << 20):
                L(f'!! ADRP вне диапазона на {place:#x} -> {S+r_add:#x}')
            w = (old & ~((0b11 << 29) | (0x7ffff << 5))) | (immlo << 29) | (immhi << 5)
            uc.mem_write(place, struct.pack('<I', w))
        elif rtype == 276:  # ADR_PREL_LO21
            off = (S + r_add - place)
            immlo = off & 3; immhi = (off >> 2) & 0x7ffff
            w = (old & ~((0b11 << 29) | (0x7ffff << 5))) | (immlo << 29) | (immhi << 5)
            uc.mem_write(place, struct.pack('<I', w))
        elif rtype == 277:  # ADD_ABS_LO12_NC
            imm12 = (S + r_add) & 0xfff
            w = (old & ~(0xfff << 10)) | (imm12 << 10)
            uc.mem_write(place, struct.pack('<I', w))
        elif rtype in (278, 284, 285, 286, 279, 280, 281):  # LDST*_ABS_LO12_NC
            scale = {278: 0, 279: 0, 280: 2, 281: 1, 284: 1, 285: 2, 286: 3}.get(rtype, 0)
            imm12 = ((S + r_add) & 0xfff) >> scale
            w = (old & ~(0xfff << 10)) | (imm12 << 10)
            uc.mem_write(place, struct.pack('<I', w))
        elif rtype == 274:  # PREL32
            v = (S + r_add - place) & 0xffffffff
            uc.mem_write(place, struct.pack('<I', v))
        elif rtype == 283:  # CALL26
            imm = (S - place) >> 2
            uc.mem_write(place, struct.pack('<I', 0x94000000 | (imm & 0x3ffffff)))
        elif rtype == 282:  # JUMP26
            if (S - place) & 3 == 0 and -(1 << 27) <= (S - place) < (1 << 27):
                imm = (S - place) >> 2
                uc.mem_write(place, struct.pack('<I', 0x14000000 | (imm & 0x3ffffff)))
            else:
                L(f'!! JUMP26 вне диапазона {place:#x} -> {S:#x}')
        elif rtype == 284:  # TLSDESC
            pass
        else:
            L(f'?? неизвестный rela type={rtype} на {place:#x}')
for i, s in enumerate(shs):
    if s['name'].startswith('.rela') and s['name'] != '.rela__jump_table' and s['name'] != '.rela.altinstructions':
        # цель: .rela.X -> X
        tn = s['name'][5:]
        if tn == 'gnu.linkonce.this_module':
            continue
        ti = next((j for j, t in enumerate(shs) if t['name'] == tn), None)
        if ti is not None:
            apply_relocs(s, ti)
# .rela.gnu.linkonce.this_module: init/exit указатели в __this_module
tm = next((j for j, t in enumerate(shs) if t['name'] == '.gnu.linkonce.this_module'), None)
if tm is not None:
    ta = SECADDR[tm] if tm in SECADDR else None
    # this_module не в ALLOC — добавим
    pass
# (this_module содержит init/exit — для эмуляции не критично)

# --- стек и хуки ---
STACK = 0x600000000000
uc.mem_map(STACK, 0x100000, UC_PROT_ALL)
SP0 = STACK + 0x80000
MAGIC_STOP = 0x52000000
uc.mem_map(MAGIC_STOP, 0x1000, UC_PROT_ALL)
uc.mem_write(MAGIC_STOP, b'\xc0\x03\x5f\xd6')  # ret

state = {'sock_reg': None, 'proto_reg': [], 'stop': False}

def hook_intr(uc, intno, data):
    x16 = uc.reg_read(UC_ARM64_REG_X16)
    x0 = uc.reg_read(UC_ARM64_REG_X0)
    x1 = uc.reg_read(UC_ARM64_REG_X1)
    x2 = uc.reg_read(UC_ARM64_REG_X2)
    x3 = uc.reg_read(UC_ARM64_REG_X3)
    if not (0 <= x16 < len(IMPORTS)):
        L(f'svc x16={x16:#x}??'); return
    name = import_names[x16]
    if name == 'sock_register':
        fam = struct.unpack('<i', bytes(uc.mem_read(x0, 4)))[0]
        # net_proto_family { int family; struct module *owner; int (*create)(); }
        owner, create = struct.unpack('<QQ', bytes(uc.mem_read(x0+8, 16)))
        L(f'sock_register(family={fam}, owner={owner:#x}, create={create:#x})')
        state['sock_reg'] = (fam, create)
        uc.reg_write(UC_ARM64_REG_X0, 0)
        return
    if name == 'proto_register':
        L(f'proto_register({x0:#x}, {x1})')
        state['proto_reg'].append(x0)
        uc.reg_write(UC_ARM64_REG_X0, 0)
        return
    if name in ('proto_unregister', 'kobject_del'):
        L(f'{name}({x0:#x}) -> 0')
        uc.reg_write(UC_ARM64_REG_X0, 0)
        return
    if name == '__list_del_entry_valid':
        uc.reg_write(UC_ARM64_REG_X0, 0); return
    if name == 'memset':
        uc.mem_write(x0, bytes([x1 & 0xff]) * x2)
        uc.reg_write(UC_ARM64_REG_X0, x0); return
    if name == '__arch_copy_from_user':
        # (to, from, n): копируем и возвращаем 0 (всё скопировано)
        try:
            uc.mem_write(x0, bytes(uc.mem_read(x1, x2)))
            uc.reg_write(UC_ARM64_REG_X0, 0)
        except Exception:
            uc.reg_write(UC_ARM64_REG_X0, x2)
        return
    if name == '__arch_copy_to_user':
        try:
            uc.mem_write(x0, bytes(uc.mem_read(x1, x2)))
            uc.reg_write(UC_ARM64_REG_X0, 0)
        except Exception:
            uc.reg_write(UC_ARM64_REG_X0, x2)
        return
    if name == '__stack_chk_fail':
        L('__stack_chk_fail! СТОП')
        state['stop'] = True; uc.emu_stop(); return
    if name in ('strstr',):
        hay = bytes(uc.mem_read(x0, 128)).split(b'\x00')[0]
        nd = bytes(uc.mem_read(x1, 128)).split(b'\x00')[0]
        found = hay.find(nd) if nd else 0
        L(f'strstr({hay!r}, {nd!r}) -> {found:#x}' if found >= 0 else f'strstr({hay!r}, {nd!r}) -> NULL')
        uc.reg_write(UC_ARM64_REG_X0, (x0 + found) if found >= 0 else 0); return
    if name in ('strcmp','strncmp'):
        a = bytes(uc.mem_read(x0, 48)).split(b'\x00')[0]
        b = bytes(uc.mem_read(x1, 48)).split(b'\x00')[0]
        r = 0 if a == b else (1 if a > b else 0xffffffffffffffff)
        if name == 'strncmp':
            n = x2; r = 0 if a[:n] == b[:n] else 1
        L(f'{name}({a!r}, {b!r}) -> {r}')
        uc.reg_write(UC_ARM64_REG_X0, r); return
    if name in ('_raw_write_lock_bh','_raw_write_unlock_bh','up_read','down_read'):
        uc.reg_write(UC_ARM64_REG_X0, 0); return
    if name in ('cpu_hwcap_keys','cpu_hwcaps'):
        uc.reg_write(UC_ARM64_REG_X0, 0); return
    # --- фейковые объекты ядра для полной трассировки ioctl ---
    if name == 'sk_alloc':
        # sk_alloc(net, family, priority, proto, kern)
        uc.reg_write(UC_ARM64_REG_X0, FAKE_SK_ADDR); return
    if name in ('sock_init_data',):
        uc.reg_write(UC_ARM64_REG_X0, 0); return
    if name == 'find_get_pid':
        L(f'find_get_pid({x0}) -> FAKE_PID')
        uc.reg_write(UC_ARM64_REG_X0, FAKE_PID_ADDR); return
    if name == 'get_pid_task':
        L(f'get_pid_task(pid={x0:#x}, type={x1}) -> FAKE_TASK')
        uc.reg_write(UC_ARM64_REG_X0, FAKE_TASK_ADDR); return
    if name == 'get_task_mm':
        L(f'get_task_mm({x0:#x}) -> FAKE_MM')
        uc.reg_write(UC_ARM64_REG_X0, FAKE_MM_ADDR); return
    if name == 'find_vma':
        L(f'find_vma(mm={x0:#x}, addr={x1:#x}) -> FAKE_VMA')
        uc.reg_write(UC_ARM64_REG_X0, FAKE_VMA_ADDR); return
    if name == 'pfn_valid':
        uc.reg_write(UC_ARM64_REG_X0, 1); return
    if name in ('put_pid', '__put_task_struct', 'mmput', 'd_path',
                '__check_object_size', 'refcount_warn_saturate'):
        if name == 'd_path' and x0:
            try: uc.mem_write(x0, b'/fake\x00')
            except Exception: pass
        uc.reg_write(UC_ARM64_REG_X0, 0); return
    # прочее — 0
    L(f'stub {name}({x0:#x}, {x1:#x}, {x2:#x}, {x3:#x}) -> 0')
    uc.reg_write(UC_ARM64_REG_X0, 0)

uc.hook_add(UC_HOOK_INTR, hook_intr)

def hook_mem_invalid(uc, access, address, size, value, ud):
    page = address & ~0xfff
    try:
        uc.mem_map(page, 0x2000, UC_PROT_ALL)
        L(f'AUTOMAP {page:#x} (access={access})')
        return True
    except Exception:
        return False
uc.hook_add(UC_HOOK_MEM_INVALID, hook_mem_invalid)

# stack guard
GUARD = 0xdeadbeef12345678
# найти адрес __stack_chk_guard (import data) — у импортов нет адреса; модуль обращается через GOT? в ядре — прямо.
# для простоты: перехват не нужен, главное init

# --- запуск init_module ---
init_sec = next(i for i, s in enumerate(shs) if s['name'] == '.init.text')
init_addr = SECADDR[init_sec]
L(f'init_module @ {init_addr:#x}, память до {MEMTOP:#x}')
uc.reg_write(UC_ARM64_REG_SP, SP0)
uc.reg_write(UC_ARM64_REG_LR, MAGIC_STOP)
try:
    uc.emu_start(init_addr, MAGIC_STOP, timeout=60*1000000, count=200_000_000)
except UcError as e:
    L(f'UC ERROR: {e} pc={uc.reg_read(UC_ARM64_REG_PC):#x}')

# --- дамп ---
img = bytes(uc.mem_read(BASE, MEMTOP - BASE))
open('/tmp/drv/ko_after_init.bin', 'wb').write(img)
L(f'дамп памяти после init: {len(img)} байт -> ko_after_init.bin')

# thunks после патчей
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
text_i = next(i for i, s in enumerate(shs) if s['name'] == '.text')
ta = SECADDR[text_i]
for name, off in [('.NMSL67',0x3340), ('.NMSL83',0x3358), ('.NMSL85',0x3350),
                  ('.NMSL43',0x32e0), ('.NMSL81',0x32e8), ('.NMSL98',0x32f8),
                  ('.NMSL97',0x32f0), ('.NMSL99',0x3318), ('.NMSL96',0x3310),
                  ('.NMSL75',0x3338), ('.NMSL54',0x3328), ('.NMSL57',0x3320),
                  ('.NMSL60',0x3330), ('.NMSL63',0x3308), ('.NMSL46',0x3300),
                  ('.NMSL78',0x32d8), ('.NMSL72',0x32d0), ('.NMSL51',0x32c8)]:
    a = ta + off
    w = struct.unpack('<I', img[a-BASE:a-BASE+4][0:4].ljust(4, b'\x00') if False else bytes(uc.mem_read(a, 4)))[0]
    w2 = struct.unpack('<I', bytes(uc.mem_read(a+4, 4)))[0]
    tgt = None
    if (w2 >> 26) == 0x05:
        imm = w2 & 0x3ffffff
        if imm & 0x2000000: imm -= 0x4000000
        tgt = (a+4) + imm*4
    L(f'{name}: {w:#010x} {w2:#010x}' + (f' -> B {tgt:#x} (.text+{tgt-ta:#x})' if tgt else ''))

# строки после расшифровки
import re
strs = [(m.start()+BASE, m.group().decode()) for m in re.finditer(rb'[\x20-\x7e]{4,}', img)]
L('=== СТРОКИ ПОСЛЕ INIT ===')
seen = set()
for off, s in strs:
    if s in seen: continue
    seen.add(s)
    L(f'{off:#x}: {s[:100]}')
open('/tmp/drv/ko_emu.log', 'w').write('\n'.join(LOG))
